#include "io/wem/wem_import.h"

#include "io/mdx_model_adapter.h"
#include "whiteout/flakes/util/path_utf8.h"

#if WDX_ENABLE_M2
#include "io/m2/m2_model_adapter.h"
#endif
#if WDX_ENABLE_M3
#include "io/m3/m3_model_adapter.h"
#endif
#if WDX_ENABLE_D3
#include <whiteout/models/wem/d3_converter.h>
#include "io/d3/d3_model_adapter.h"
#include "io/d3/d3_sno_cache.h"
#endif

#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/parser.h>
#include <whiteout/models/wem/retarget.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <utility>
#include <variant>

namespace whiteout::flakes::io {

namespace {

/// The `.m2` header version the block records, or the parser's own default.
/// `M2Converter::fromM2` puts it on the set because `m2::Model` does not carry
/// one — the parser consumes it and keeps nothing.
u32 M2VersionOf(const wem::Model& model, wem::ProfileId profile) {
    if (const wem::ProfileMaterialSet* set = model.setFor(profile))
        return static_cast<u32>(set->native.value("sourceVersion", 274));
    return 274;
}

/// The `MODL` version, which decides everything downstream: v30+ is Heroes,
/// where MADD is the load-time truth. A derived set carries no `modelVersion`,
/// so the profile answers instead — the same rule `ProfileForVersion` states,
/// read backwards.
u32 M3VersionOf(const wem::Model& model, wem::ProfileId profile) {
    if (const wem::ProfileMaterialSet* set = model.setFor(profile)) {
        const i64 stored = set->native.value("modelVersion", 0);
        if (stored > 0)
            return static_cast<u32>(stored);
    }
    return profile == wem::ProfileId::Heroes ? 30u : 29u;
}

/// Give a texture MDX cannot name the one key the host can still resolve.
///
/// MDX addresses a texture by file name and by nothing else, so a document that
/// addresses its own by ID — a Diablo III SNO, a World of Warcraft fileDataID —
/// converts to a model of empty names, and an empty name stages a 4x4
/// placeholder per slot. That is a white Barbarian on a Warcraft III grid.
///
/// `#<id>` is the host's spelling of "resolve this by id" — the same key
/// `D3ModelAdapter` and `M2ModelAdapter` hand out and `ModelLoader` turns back
/// into a `ContentRef::FromFileId`. A real `.mdx` on disk could not use it, and
/// that is fine: this model is never written, and the alternative is no
/// reference at all.
///
/// `toMdx` writes one MDX texture per document texture in order, so the index
/// is the join.
void NameIdTextures(const wem::Document& document, ::whiteout::mdx::Model& out) {
    for (usize i = 0; i < out.textures.size() && i < document.textures.size(); ++i) {
        if (!out.textures[i].fileName.empty())
            continue;
        const wem::TextureRef& ref = document.textures[i];
        if (const auto* sno = std::get_if<wem::TextureSnoId>(&ref.key))
            out.textures[i].fileName = "#" + std::to_string(sno->id);
        else if (const auto* fileId = std::get_if<wem::TextureFileDataId>(&ref.key))
            out.textures[i].fileName = "#" + std::to_string(fileId->value);
    }
}

} // namespace

bool LooksLikeWem(std::span<const ::whiteout::u8> data) {
    return wem::IsWemFile(data);
}

std::shared_ptr<WemDocument> ParseWemDocument(std::span<const ::whiteout::u8> data,
                                              std::string name) {
    if (!LooksLikeWem(data))
        return nullptr;

    wem::Parser parser;
    std::optional<wem::Document> parsed = parser.parse(data);
    if (!parsed)
        return nullptr;

    auto out = std::make_shared<WemDocument>();
    out->document = std::move(*parsed);
    out->diagnostics = parser.diagnostics();
    out->name = std::move(name);
    // The chunks this build did not understand ride back out on the document
    // (§11.4), so a read-edit-write round trip preserves them by doing nothing.
    // The parser hands them over separately; putting them where the writer
    // looks is this one line.
    if (out->document.unknownChunks.empty() && !parser.unknownChunks().empty())
        out->document.unknownChunks = parser.unknownChunks();
    return out;
}

std::shared_ptr<WemDocument> ParseWemFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return nullptr;
    std::vector<::whiteout::u8> bytes((std::istreambuf_iterator<char>(file)),
                                      std::istreambuf_iterator<char>());
    if (bytes.empty())
        return nullptr;
    return ParseWemDocument(bytes, PathToUtf8(path.filename()));
}

std::string DescribeWemDiagnostics(const wem::Diagnostics& diagnostics, usize maxLines) {
    if (diagnostics.empty())
        return {};
    std::string out;
    usize drawn = 0;
    for (const wem::Diagnostic& entry : diagnostics.all()) {
        if (drawn >= maxLines) {
            out += "  ... and " + std::to_string(diagnostics.size() - drawn) + " more\n";
            break;
        }
        out += "  [";
        out += wem::ToString(entry.severity);
        out += "] ";
        out += wem::ToString(entry.code);
        out += ": ";
        out += entry.message;
        out += '\n';
        ++drawn;
    }
    return out;
}

WemStagingResult StageWemDocument(const wem::Document& source, wem::ProfileId profile,
                                  wem::Document& scratch, const WemStagingOptions& options) {
    WemStagingResult result;
    result.rescaled = 1.0f;

    // The copy is made only when something actually has to change. A document
    // is large — every mesh's attribute layers are in it — and opening a file as
    // the profile it already carries should not pay for that.
    const wem::Document* staged = &source;
    const auto mutate = [&]() -> wem::Document& {
        if (staged != &scratch) {
            scratch = *staged;
            staged = &scratch;
        }
        return scratch;
    };

    if (!source.carries(profile)) {
        const wem::ProfileId from = WemDeriveSource(source, profile);
        if (from == wem::ProfileId::Count) {
            result.error = std::string("declares no profile to derive ") +
                           wem::Profile(profile).displayName + " from";
            return result;
        }
        wem::Document& document = mutate();
        document.declare(profile);
        const wem::DeriveResult derive = wem::DeriveProfile(document, from, profile);
        result.diagnostics.append(derive.diagnostics);
        if (!derive.ok) {
            result.error = std::string("could not be derived as ") +
                           wem::Profile(profile).displayName + " from " +
                           wem::Profile(from).displayName;
            return result;
        }
        result.derived = true;
    }

    // The rig convention, which a converter only WARNS about (§10.5): restating
    // a skeleton edits the document, and a read of one may not do that
    // silently. Nothing else in the host ran it, so a Diablo III or StarCraft
    // II document — both `ExplicitBind` — reached `toMdx` with pivots composed
    // from its rest chain and node tracks still keyed against the bind frame
    // those pivots just replaced, which poses the skeleton at neither.
    const wem::RigConvention wantedRig = wem::Profile(profile).rig;
    const bool restateRig =
        std::any_of(staged->models.begin(), staged->models.end(), [wantedRig](const wem::Model& m) {
            return !m.nodes.empty() && m.nodes.rig != wantedRig;
        });
    if (restateRig) {
        const wem::SkeletonRetargetResult rig = wem::RetargetSkeleton(mutate(), profile);
        result.diagnostics.append(rig.diagnostics);
        if (!rig.ok) {
            result.error =
                std::string("could not be restated as a ") + wem::ToString(wantedRig) + " rig";
            return result;
        }
    }

    // Before the rescale, so the numbers the rescale reports are the numbers
    // that were written. Warcraft III has no geoset level of detail below
    // `.mdx` v1000 and no use for one above it — a map's models are drawn at
    // one distance — so the ladder is dropped rather than carried.
    if (options.baseLodOnly) {
        wem::Document* document = nullptr;
        for (const wem::Model& model : staged->models) {
            const bool any = std::any_of(model.meshes.begin(), model.meshes.end(),
                                         [](const wem::Mesh& m) { return m.lodLevel != 0; });
            if (any) {
                document = &mutate();
                break;
            }
        }
        if (document != nullptr) {
            for (wem::Model& model : document->models) {
                const auto base = static_cast<u32>(
                    std::count_if(model.meshes.begin(), model.meshes.end(),
                                  [](const wem::Mesh& mesh) { return mesh.lodLevel == 0; }));
                // Counted before anything moves, and never all of them: a model
                // whose every mesh claims a non-zero level would come out empty,
                // and an empty model is a worse answer than a coarse one.
                if (base == 0 || base == model.meshes.size())
                    continue;
                result.lodMeshesDropped += static_cast<u32>(model.meshes.size()) - base;
                model.meshes.erase(
                    std::remove_if(model.meshes.begin(), model.meshes.end(),
                                   [](const wem::Mesh& mesh) { return mesh.lodLevel != 0; }),
                    model.meshes.end());
            }
            if (result.lodMeshesDropped != 0) {
                result.diagnostics.info(wem::DiagCode::LevelOfDetailDropped,
                                        std::to_string(result.lodMeshesDropped) +
                                            " mesh(es) above the base level of detail were not "
                                            "carried; Warcraft III draws every geoset it is given",
                                        wem::ElementRef(), profile);
            }
        }
    }

    // Last, so the retarget above measures its residual in the units the model
    // was authored in — and because a uniform scale commutes with everything
    // either step does, which is what makes the order a matter of reporting
    // rather than of correctness.
    if (options.rescale != 1.0f) {
        const wem::RescaleResult rescale = wem::RescaleDocument(mutate(), options.rescale);
        result.diagnostics.append(rescale.diagnostics);
        if (!rescale.ok) {
            result.error = "could not be rescaled";
            return result;
        }
        result.rescaled = options.rescale;
    }

    result.document = staged;
    return result;
}

WemSourceResult BuildWemSource(const WemDocument& parsed, wem::ProfileId profile,
                               const std::filesystem::path& basePath, IContentProvider* provider,
                               D3SnoCache* d3Cache) {
    WemSourceResult result;

    if (profile == wem::ProfileId::Count)
        profile = DefaultWemProfile(parsed.document);
    if (profile == wem::ProfileId::Count) {
        result.error = "'" + parsed.name + "' carries no profile this build can open";
        return result;
    }
    if (!WemProfileOpenable(profile)) {
        result.error = std::string("cannot open as ") + wem::Profile(profile).displayName + ": " +
                       WemProfileUnsupportedReason(profile);
        return result;
    }
    if (parsed.document.models.empty()) {
        result.error = "'" + parsed.name + "' holds no model";
        return result;
    }

    result.profile = profile;
    result.product = ProductForWemProfile(profile);
    result.hd = WemProfileIsHd(profile);
    // The profile the GEOMETRY and the texture keys belong to, which is not
    // necessarily the one being opened: the derive below restates a material
    // set and never touches a vertex or a key. `defaultProfile` rather than
    // the derive source, because a document that carries both sets still has
    // its geometry in the units of the game it came from — every importer
    // stamps this and nothing else answers the question.
    const wem::ProfileId authored = parsed.document.defaultProfile;
    result.assetProduct = ProductForWemProfile(authored);
    result.worldScale = wem::Profile(authored).sceneScale;

    // An open leaves the geometry in the units it was authored in and stamps
    // `worldScale` above; only a WRITTEN file has to be rescaled, which is what
    // the export path asks the staging for.
    wem::Document stagedDocument;
    const WemStagingResult staged = StageWemDocument(parsed.document, profile, stagedDocument);
    result.diagnostics.append(staged.diagnostics);
    if (!staged.ok()) {
        result.error = "'" + parsed.name + "' " + staged.error;
        return result;
    }
    result.derived = staged.derived;
    const wem::Document* source = staged.document;

    // One converter per format, chosen by the profile's `formatId` — the same
    // join `ConverterRegistry::findForProfile` makes, spelled out because each
    // arm needs its own typed entry point and its own adapter.
    const wem::ProfileDesc& desc = wem::Profile(profile);
    // Every openable profile names a format; `WemProfileOpenable` refuses the
    // one that does not (`Generic`), because a converter refuses a profile it
    // does not serve and `MdxConverter` serves the two WC3 ids by name.
    const std::string formatId = desc.formatId ? desc.formatId : "";

    if (formatId == "mdx") {
        wem::MdxConverter converter;
        wem::Result<::whiteout::mdx::Model> converted =
            converter.toMdx(*source, profile, MdxVersionForWemProfile(profile));
        result.diagnostics.append(converted.diagnostics);
        if (!converted.ok()) {
            result.error = "converting '" + parsed.name + "' to a Warcraft III model failed";
            return result;
        }
        ::whiteout::mdx::Model model = converted.take();
        NameIdTextures(*source, model);
        result.source = std::make_shared<MdxModelAdapter>(std::move(model), basePath, provider);
        return result;
    }

#if WDX_ENABLE_M2
    if (formatId == "m2") {
        wem::M2Converter converter;
        wem::Result<::whiteout::m2::Model> converted =
            converter.toM2(*source, profile, M2VersionOf(source->models.front(), profile));
        result.diagnostics.append(converted.diagnostics);
        if (!converted.ok()) {
            result.error = "converting '" + parsed.name + "' to a World of Warcraft model failed";
            return result;
        }
        result.source = std::make_shared<M2ModelAdapter>(converted.take());
        return result;
    }
#endif

#if WDX_ENABLE_M3
    if (formatId == "m3") {
        wem::M3Converter converter;
        wem::Result<::whiteout::m3::Model> converted =
            converter.toM3(*source, profile, M3VersionOf(source->models.front(), profile));
        result.diagnostics.append(converted.diagnostics);
        if (!converted.ok()) {
            result.error = "converting '" + parsed.name + "' to a StarCraft II model failed";
            return result;
        }
        result.source = std::make_shared<M3ModelAdapter>(converted.take());
        return result;
    }
#endif

#if WDX_ENABLE_D3
    if (formatId == "d3") {
        // The only arm that needs somewhere to put what it built. A
        // `D3ModelAdapter` reads its appearance, and later every `.ani` a tag
        // names, through a `D3SnoCache` — so the converted assets are adopted
        // into one rather than handed over directly, and the adapter's own
        // lazy-clip path then works unchanged.
        if (d3Cache == nullptr) {
            result.error = "opening a Diablo III model needs the scene's SNO cache";
            return result;
        }
        wem::D3Converter converter;
        wem::D3ExportOptions options;
        wem::Result<wem::D3AppearanceExport> converted = converter.toAppearance(*source, options);
        result.diagnostics.append(converted.diagnostics);
        if (!converted.ok()) {
            result.error = "converting '" + parsed.name + "' to a Diablo III model failed";
            return result;
        }

        const u32 look = converted.value->look;
        std::vector<u8> hidden = std::move(converted.value->hidden);
        // A `.wem` that carries no appearance id -- one derived from another
        // profile -- still needs one the cache can key on, and a negative id is
        // no id at all.
        i32 appearanceSno = converted.value->appearance.dwSnoId;
        if (appearanceSno <= 0) {
            appearanceSno = wem::d3SyntheticAppearanceId;
            converted.value->appearance.dwSnoId = appearanceSno;
        }
        auto appearance =
            d3Cache->AdoptAppearance(appearanceSno, std::move(converted.value->appearance));
        auto adapter = std::make_shared<D3ModelAdapter>(appearance, look);

        // The dressing, all three parts of it. None is in a `.app` — which
        // armour draws, which look each piece resolves under and what it is
        // dyed are equipment state the engine applies — so the document's own
        // answers are re-applied here or every armour variant draws at once, in
        // the model's default look, undyed.
        if (!hidden.empty())
            adapter->SetGeosetHidden(std::move(hidden));
        if (!converted.value->looks.empty()) {
            std::vector<u32> perGeoset = std::move(converted.value->looks);
            for (u32& entry : perGeoset) {
                if (entry == wem::kInvalidIndex)
                    entry = look;
            }
            adapter->SetGeosetLooks(std::move(perGeoset));
        }
        if (!converted.value->dyes.empty())
            adapter->SetGeosetDyes(std::move(converted.value->dyes));

        wem::Result<wem::D3Converter::D3AnimExport> anims = converter.toAnimSet(*source, 0);
        result.diagnostics.append(anims.diagnostics);
        if (anims.ok() && !anims.value->anims.empty()) {
            for (::whiteout::sno::d3::native::Anim& anim : anims.value->anims) {
                const i32 sno = anim.dwSnoId;
                d3Cache->AdoptAnim(sno, std::move(anim));
            }
            i32 setSno = anims.value->animSet.dwSnoId;
            if (setSno <= 0) {
                setSno = wem::d3SyntheticAnimSetId;
                anims.value->animSet.dwSnoId = setSno;
            }
            auto set = d3Cache->AdoptAnimSet(setSno, std::move(anims.value->animSet));
            // Not lazy: every clip is already in memory, and the lazy path
            // exists to keep a browser from parsing 259 `.ani` files per cell.
            adapter->BindAnimations(*d3Cache, std::move(set), /*lazy=*/false);
        }

        result.source = std::move(adapter);
        return result;
    }
#else
    (void)d3Cache;
#endif

    // WemProfileOpenable already refused everything that reaches here, so this
    // is the "the two disagreed" branch rather than a user-visible path.
    result.error = std::string("no converter for format '") + formatId + "'";
    return result;
}

} // namespace whiteout::flakes::io
