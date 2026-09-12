#include "io/wem/wem_export.h"

#include "io/wem/wem_import.h"
#include "io/wem/wem_profiles.h"

#include "io/mdx_model_adapter.h"
#include "whiteout/flakes/util/path_utf8.h"

#if WDX_ENABLE_M2
#include "io/m2/m2_model_adapter.h"
#endif
#if WDX_ENABLE_M3
#include "io/m3/m3_model_adapter.h"
#include <whiteout/models/m3/engine_compat.h>
#endif
#if WDX_ENABLE_D3
#include <whiteout/models/wem/d3_converter.h>
#include "io/d3/d3_asset_provider.h"
#include "io/d3/d3_model_adapter.h"
#endif

#include <whiteout/models/wem/converters.h>
#include <whiteout/models/wem/retarget.h>
#include <whiteout/models/wem/writer.h>

#include <cstdlib>
#include <span>
#include <string>
#include <utility>

namespace whiteout::flakes::io {

namespace {

/// The `.m2` header version the block records.
///
/// `m2::Model` does not carry one — the parser consumes it and keeps nothing —
/// and neither does the adapter, so the writer states the parser's own default.
/// A caller that knows better cannot say so today; when the adapter starts
/// keeping the version, this is the one line that changes.
constexpr u32 kM2SourceVersion = 274;

#if WDX_ENABLE_M2
/// Write what the *spawn* resolved into the slots the file leaves blank.
///
/// A World of Warcraft creature names none of its own skins: the slot carries a
/// texture *type* (11/12/13 for a creature, 1/6/8 for a character) and the game
/// fills it from the display record at spawn. `fromM2` sees only the parsed
/// model, so those refs come back keyed on nothing and the document draws
/// white — which is exactly what a `.wem` must not do, since its whole premise
/// is that it names its own textures.
///
/// `replaceableId`/`slotType` are left alone: the reference still records that
/// this slot *is* replaceable, and a host that wants to restyle the model has
/// everything it needs. Only the key becomes concrete.
void BakeM2Replaceables(wem::Document& document, const M2ModelAdapter& adapter,
                        wem::Diagnostics& diagnostics) {
    const std::span<const std::string> resolved = adapter.ReplaceableTextures();
    for (usize i = 0; i < document.textures.size(); ++i) {
        wem::TextureRef& ref = document.textures[i];
        if (wem::KeyKind(ref.key) != wem::TextureKeyKind::None || ref.slotType == 0)
            continue;
        const std::string key =
            ref.slotType < resolved.size() ? resolved[ref.slotType] : std::string();
        if (key.empty()) {
            // A character body composite lands here: the game builds those
            // pixels at load and no file holds them, so there is no key to
            // write. Saying so beats a silently white model.
            diagnostics.warn(wem::DiagCode::TextureUnresolved,
                             "texture type " + std::to_string(ref.slotType) +
                                 " is filled by the game and nothing resolved it; "
                                 "the slot is written blank",
                             wem::ElementRef(wem::ElementKind::Texture, static_cast<u32>(i)));
            continue;
        }
        // `#<id>` is ContentRef::Describe's spelling of a fileDataID, which is
        // what WowReplaceableTextures hands back for retail content; anything
        // else is a path.
        if (key.size() > 1 && key[0] == '#') {
            ref.key = wem::TextureFileDataId{
                static_cast<u32>(std::strtoul(key.c_str() + 1, nullptr, 10))};
        } else {
            ref.key = wem::TexturePath{key};
            ref.path = key;
        }
    }
}
#endif

#if WDX_ENABLE_M3
/// Bake the `.m3a` files the host attached into the document's clips.
///
/// StarCraft II keeps a good deal of animation outside the model: 447 `.m3a`
/// files ship beside the StarCraft II corpus and 663 beside Heroes', and 429 of
/// the 33,060 StarCraft II models carry no sequence of their own at all — a
/// cinematic actor like `SM_RaynorMarine` moves entirely through attached
/// files. The game pairs them by catalog entry (`CModel.RequiredAnims`); we
/// have no catalog, so the pairing is the host's — the `Anims` button,
/// `--attach-anim`. That makes an attached file as much a part of what is on
/// screen as the Diablo III dressing below, and an export that read only the
/// model would write one that cannot move.
///
/// The join inside is `animId` alone, so this adds motion for channels the
/// document already declares and skips the rest; `mergeAnimation` reports how
/// many of each file's sequences landed.
void BakeM3ExternalAnimations(const M3ModelAdapter& adapter, const wem::M3Converter& converter,
                              wem::Document& document, wem::Diagnostics& diagnostics) {
    const std::span<const M3ModelAdapter::AttachedAnimation> attached =
        adapter.AttachedAnimations();
    if (attached.empty() || document.models.empty())
        return;
    for (usize i = 0; i < attached.size(); ++i) {
        const ::whiteout::m3::Model* animation = adapter.AttachedAnimationModel(i);
        if (animation == nullptr)
            continue;
        // `fromM3` builds a one-model document, so the target is index 0.
        wem::Result<u32> merged = converter.mergeAnimation(document, 0, *animation);
        diagnostics.append(merged.diagnostics);
        diagnostics.info(wem::DiagCode::Unspecified,
                         "external animation '" + attached[i].label + "': " +
                             std::to_string(merged.value.value_or(0)) + " of " +
                             std::to_string(attached[i].sequenceCount) + " sequence(s) merged");
    }
}
#endif

#if WDX_ENABLE_D3
/// Stamp the host's dressing onto the sections the converter just built.
///
/// Which armour draws is **equipment state**: `ActorModel_ApplyLook` flips
/// visibility on sub-objects that were there all along, and the answer lives on
/// `D3CharacterAppearance` and reaches the adapter as `SetGeosetHidden`. The
/// converter reads the `.app` and sees none of it, so a dressed character would
/// export naked — the same argument the look already makes, one level down.
///
/// The join is the adapter's own emission order: a `D3SubObjectRef` names the
/// geoset and the index within it, which are exactly the mesh and the section
/// the import created.
void BakeD3Dressing(const D3ModelAdapter& adapter, wem::Document& document) {
    const std::span<const u8> hidden = adapter.GeosetHidden();
    const std::span<const u32> looks = adapter.GeosetLooks();
    const std::span<const i32> dyes = adapter.GeosetDyes();
    if (hidden.empty() && looks.empty() && dyes.empty())
        return;

    // The actor's own appearance, not a child model riding a hardpoint.
    const u32 model = wem::D3Converter::FindAppearanceModel(document, adapter.AppearanceSno());
    if (model == wem::kInvalidIndex || model >= document.models.size())
        return;
    wem::Model& target = document.models[model];

    const std::span<const D3SubObjectRef> emitted = adapter.EmittedSubObjects();
    for (usize g = 0; g < emitted.size(); ++g) {
        const usize mesh = emitted[g].geoSet;
        const usize section = emitted[g].index;
        if (mesh >= target.meshes.size() || section >= target.meshes[mesh].sections.size())
            continue;
        wem::MeshSection& out = target.meshes[mesh].sections[section];
        if (g < hidden.size()) {
            // Assigned, never or-ed in: the converter already set the flag from
            // its own default wardrobe, and the host's answer replaces it.
            const u32 flags = static_cast<u32>(out.flags);
            const u32 bit = static_cast<u32>(wem::SectionFlags::Hidden);
            out.flags = static_cast<wem::SectionFlags>(hidden[g] ? (flags | bit) : (flags & ~bit));
        }
        // The two per-piece overrides. Their defaults are "the model's look"
        // and "undyed", so an entry appears only where something overrode.
        if (g < looks.size() && looks[g] != adapter.LookIndex())
            out.native.set("lookOverride", static_cast<i64>(looks[g]));
        if (g < dyes.size() && dyes[g] != 0)
            out.native.set("dye", static_cast<i64>(dyes[g]));
    }
}
#endif

} // namespace

bool CanExportModelToWem(const renderer::model::IModelSource& source) {
    if (dynamic_cast<const MdxModelAdapter*>(&source))
        return true;
#if WDX_ENABLE_M2
    if (dynamic_cast<const M2ModelAdapter*>(&source))
        return true;
#endif
#if WDX_ENABLE_M3
    if (dynamic_cast<const M3ModelAdapter*>(&source))
        return true;
#endif
#if WDX_ENABLE_D3
    if (dynamic_cast<const D3ModelAdapter*>(&source))
        return true;
#endif
    return false;
}

WemExportResult ExportModelToWem(renderer::model::IModelSource& source, IContentProvider* provider,
                                 const WemExportOptions& options) {
    WemExportResult result;

    const auto finish = [&options](WemExportResult& out) {
        if (out.document && !options.documentName.empty())
            out.document->name = options.documentName;
    };

    if (auto* mdx = dynamic_cast<MdxModelAdapter*>(&source)) {
        result.formatId = "mdx";
        wem::MdxConverter converter;
        // One import, up to two material sets: the classic/Reforged split is
        // per LAYER on disk (§7.2.1), so a file carrying both produces both and
        // a classic-only file produces no Reforged set at all.
        wem::Result<wem::Document> converted = converter.fromMdx(mdx->SourceModel());
        result.diagnostics = std::move(converted.diagnostics);
        if (!converted.ok()) {
            result.error = "the Warcraft III model did not convert";
            return result;
        }
        result.document = converted.take();
        finish(result);
        return result;
    }

#if WDX_ENABLE_M2
    if (auto* m2 = dynamic_cast<M2ModelAdapter*>(&source)) {
        result.formatId = "m2";
        wem::M2Converter converter;
        wem::Result<wem::Document> converted =
            converter.fromM2(m2->SourceModel(), kM2SourceVersion);
        result.diagnostics = std::move(converted.diagnostics);
        if (!converted.ok()) {
            result.error = "the World of Warcraft model did not convert";
            return result;
        }
        result.document = converted.take();
        BakeM2Replaceables(*result.document, *m2, result.diagnostics);
        finish(result);
        return result;
    }
#endif

#if WDX_ENABLE_M3
    if (auto* m3 = dynamic_cast<M3ModelAdapter*>(&source)) {
        result.formatId = "m3";
        wem::M3Converter converter;

        // The profile follows the MODL version — v30+ is Heroes — and the
        // adapter has not touched it, so the converter's own rule stands. It is
        // read here rather than left to `fromM3` because the retarget below
        // lowers the version to v29 and the answer would change: a Heroes model
        // written out for StarCraft II is still Heroes *content*, and its
        // textures belong under `Heroes\`.
        const whiteout::m3::Model& authored = m3->SourceModel();
        const wem::ProfileId profile = wem::M3Converter::ProfileForVersion(
            authored.getVersion() < 0 ? 30u : static_cast<u32>(authored.getVersion()));

        // Heroes of the Storm's half of MD34, rewritten as StarCraft II's.
        // Everything downstream of here — the derive, the surface crossing in
        // `sc2_pbr_export.cpp`, `exportPbr` — is written against a
        // `StandardMaterial`, and a shader-graph material is not one. Reversing
        // it at the model level does the job once, in the one place that knows
        // how, and reports per material what the reversal could not carry.
        whiteout::m3::EngineConversion retargeted;
        const whiteout::m3::Model* model = &authored;
        if (options.retargetHeroesToStarCraft2 && whiteout::m3::isHeroesOnly(authored)) {
            retargeted = whiteout::m3::toStarCraft2(authored);
            if (retargeted.converted) {
                model = &retargeted.model;
                for (const std::string& loss : retargeted.lossy) {
                    result.diagnostics.info(wem::DiagCode::LossyKindConversion,
                                            "Heroes -> StarCraft II: " + loss);
                }
            } else {
                // Not fatal: the converter reverses a MADD per material and
                // keeps the blob beside whatever it gets, so a model this
                // refuses whole still converts in part. Saying so is the point
                // — the material it names is the one that will come out blank.
                result.diagnostics.warn(wem::DiagCode::LossyKindConversion,
                                        "this Heroes model has no StarCraft II form (" +
                                            retargeted.blocker +
                                            "); converting it as authored instead");
            }
        }

        wem::Result<wem::Document> converted = converter.fromM3(*model, profile);
        result.diagnostics.append(converted.diagnostics);
        if (!converted.ok()) {
            result.error = "the StarCraft II model did not convert";
            return result;
        }
        result.document = converted.take();
        // What the host attached is what the model animates by — see
        // BakeM3ExternalAnimations. Nothing in the `.m3` could have named it.
        BakeM3ExternalAnimations(*m3, converter, *result.document, result.diagnostics);
        finish(result);
        return result;
    }
#endif

#if WDX_ENABLE_D3
    if (auto* d3 = dynamic_cast<D3ModelAdapter*>(&source)) {
        result.formatId = "d3";
        wem::D3Converter converter;

        wem::D3ImportOptions d3Options;
        d3Options.importAnimation = options.importAnimation;
        d3Options.materialLook = options.materialLook;
        if (d3Options.materialLook.empty()) {
            // What the user is looking at IS what "export this" means. The
            // adapter's look index is the engine's weighted pick unless the
            // host changed it, and either way it is the one on screen.
            const std::span<const std::string> looks = d3->Looks();
            if (d3->LookIndex() < looks.size())
                d3Options.materialLook = looks[d3->LookIndex()];
        }

        // The materials live on assets the appearance only names — a ShaderMap
        // and two whole Shaders per sub-object — so without a provider the
        // render state is simply missing. That is a degraded export rather than
        // a failed one, which is why the null case still runs.
        std::unique_ptr<D3AssetProvider> assetProvider;
        std::unique_ptr<wem::AssetSource> assets;
        if (provider) {
            assetProvider = std::make_unique<D3AssetProvider>(provider);
            assets = std::make_unique<wem::AssetSource>(*assetProvider);
        }

        // An actor is the unit where there is one (§9.1): it carries the attach
        // points, the anim set and the look, none of which the appearance
        // states. A browsed `.app` has no actor and converts as parts.
        wem::Result<wem::Document> converted =
            (d3->SourceActor() && assets)
                ? converter.fromActor(*d3->SourceActor(), *assets, d3Options)
                : converter.fromAppearance(d3->SourceAppearance(), assets.get(), d3Options);
        result.diagnostics = std::move(converted.diagnostics);
        if (!converted.ok()) {
            result.error = "the Diablo III model did not convert";
            return result;
        }
        result.document = converted.take();
        // **The dressing is what the user is looking at**, and the converter
        // cannot see it: which armour draws is equipment state the host applied
        // with `SetGeosetHidden`, not anything the `.app` says. Without this a
        // dressed Barbarian exports naked — the same argument the look above
        // already makes, one level down.
        BakeD3Dressing(*d3, *result.document);
        finish(result);
        return result;
    }
#endif

    result.error = "this model did not come from a format WEM can be written from";
    return result;
}

MdxExportResult ConvertWemToMdx(const wem::Document& document, const MdxExportOptions& options) {
    MdxExportResult result;

    const u32 version = MdxVersionForWemProfile(options.profile);
    if (version == 0) {
        result.error = std::string(wem::Profile(options.profile).displayName) +
                       " is not a Warcraft III profile";
        return result;
    }

    WemStagingOptions staging;
    staging.baseLodOnly = options.baseLodOnly;
    if (options.rescale) {
        // The profile the GEOMETRY belongs to, which is not the one being
        // written: the derive below restates a material set and never touches a
        // vertex. `defaultProfile` is what every importer stamps and the only
        // thing that answers the question.
        staging.rescale = wem::RescaleFactorBetween(document.defaultProfile, options.profile);
    }

    wem::Document scratch;
    const WemStagingResult staged = StageWemDocument(document, options.profile, scratch, staging);
    result.diagnostics.append(staged.diagnostics);
    if (!staged.ok()) {
        result.error = "this model " + staged.error;
        return result;
    }
    result.derived = staged.derived;
    result.scale = staged.rescaled;
    result.lodMeshesDropped = staged.lodMeshesDropped;

    wem::MdxConverter converter;
    wem::Result<::whiteout::mdx::Model> converted =
        converter.toMdx(*staged.document, options.profile, version);
    result.diagnostics.append(converted.diagnostics);
    if (!converted.ok()) {
        result.error = "the conversion to a Warcraft III model failed";
        return result;
    }
    result.model = converted.take();
    return result;
}

M3ExportResult ConvertWemToM3(const wem::Document& document, const M3ExportOptions& options) {
    M3ExportResult result;

    // One container, two version ranges: a v30 file imports back as Heroes of
    // the Storm (`ProfileForVersion`), so a StarCraft II export must stay at
    // the game's own ceiling.
    u32 version = 0;
    if (options.profile == wem::ProfileId::Sc2) {
        version = 29;
    } else if (options.profile == wem::ProfileId::Heroes) {
        version = 30;
    }
    if (version == 0) {
        result.error = std::string(wem::Profile(options.profile).displayName) +
                       " is not a StarCraft II profile";
        return result;
    }

    WemStagingOptions staging;
    staging.baseLodOnly = options.baseLodOnly;
    if (options.rescale) {
        staging.rescale = wem::RescaleFactorBetween(document.defaultProfile, options.profile);
    }

    wem::Document scratch;
    const WemStagingResult staged = StageWemDocument(document, options.profile, scratch, staging);
    result.diagnostics.append(staged.diagnostics);
    if (!staged.ok()) {
        result.error = "this model " + staged.error;
        return result;
    }
    result.derived = staged.derived;
    result.scale = staged.rescaled;
    result.lodMeshesDropped = staged.lodMeshesDropped;

    wem::M3Converter converter;
    wem::M3ExportSettings settings;
    settings.exactPasses = options.exactPasses;
    settings.textureAlphaClasses = options.textureAlphaClasses;
    settings.effectNodeBones = options.effectNodeBones;
    wem::Result<::whiteout::m3::Model> converted =
        converter.toM3(*staged.document, options.profile, version, settings, &result.map);
    result.diagnostics.append(converted.diagnostics);
    if (!converted.ok()) {
        result.error = "the conversion to a StarCraft II model failed";
        return result;
    }
    result.model = converted.take();
    if (options.keepStagedDocument)
        result.staged = *staged.document;
    return result;
}

bool WriteWemDocument(const wem::Document& document, const std::filesystem::path& path,
                      wem::Diagnostics* diagnostics, std::string* error) {
    wem::Writer writer;
    const bool ok = writer.write(PathToUtf8(path), document);
    if (diagnostics)
        diagnostics->append(writer.diagnostics());
    if (!ok && error)
        *error = "could not write '" + PathToUtf8(path) + "'";
    return ok;
}

} // namespace whiteout::flakes::io
