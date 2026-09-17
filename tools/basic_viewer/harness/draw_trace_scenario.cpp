#include "harness/draw_trace_scenario.h"

#include "harness/harness_common.h"
#include "string_util.h"

#include "renderer/model/model_instance.h"
#include "renderer/model/model_loader.h"
#include "renderer/particle/particle_service.h"
#include "renderer/render_service.h"
#include "renderer/ribbon/ribbon_service.h"
#include "renderer/scene_manager.h"
#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/util/path_utf8.h"
#if WDX_ENABLE_M3
#include "io/m3/m3_model_adapter.h"
#include "renderer/particle/particle_adapters.h"
#endif
#if WDX_ENABLE_D3
#include "io/d3/d3_model_adapter.h"
#include "renderer/profiles/diablo3/d3_surface_table.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

namespace whiteout::flakes::harness::draw_trace {

namespace model = renderer::model;

i32 ResolveSequenceSpec(const std::vector<SequenceInfo>& seqs, const std::string& spec) {
    if (spec.empty() || seqs.empty())
        return -1;
    if (spec.find_first_not_of("0123456789") == std::string::npos) {
        const i32 idx = std::atoi(spec.c_str());
        return (idx >= 0 && idx < static_cast<i32>(seqs.size())) ? idx : -1;
    }
    // SC2 sequence names are "Attack 02", and the corpus file separates its
    // scenario tokens on whitespace.
    std::string needle = spec;
    std::replace(needle.begin(), needle.end(), '+', ' ');
    for (usize i = 0; i < seqs.size(); ++i) {
        if (tools::ContainsIgnoreCase(seqs[i].name, needle))
            return static_cast<i32>(i);
    }
    return -1;
}

void ApplyD3Outfit(renderer::RenderService& renderer, renderer::SceneManager& scene, Actors spawned,
                   const cli::AnimScenario& anim, const std::filesystem::path& modelPath,
                   const std::string& contentRoot) {
#if WDX_ENABLE_D3
    if (anim.d3Equip.empty() || spawned.empty())
        return;
    model::Actor& hero = *spawned.front();
    auto adapter = std::dynamic_pointer_cast<io::D3ModelAdapter>(hero.animation.Source());
    if (!adapter) {
        std::cerr << "[dtrace] --d3-equip needs a D3 model" << std::endl;
        return;
    }
    // Registry and actors come from the content root's corpus tree
    // (GameBalance/ + Actor/), or from the provider when one can answer.
    auto& items = renderer.Loader().D3Items();
    if (!contentRoot.empty()) {
        const auto root = io::FsPathFromUtf8(contentRoot);
        items.SetFallbackDirectory(root / "GameBalance");
        items.SetFallbackActorDirectory(root / "Actor");
        items.SetFallbackStringListDirectory(root / "StringList");
    }
    items.EnsureBuilt(scene.ActiveContentProvider());
    auto& chars = renderer.Loader().D3Characters();
    if (const auto body = io::d3n::playerFromAppearanceStem(io::PathToUtf8(modelPath.stem())))
        chars.SetOutfitBody(*adapter, body->first, body->second);
    chars.SetOutfitSheathed(*adapter, anim.d3Sheathed);
    for (const auto& [slot, name] : anim.d3Equip) {
        const io::D3ItemRecord* rec = items.FindByName(name);
        std::shared_ptr<const io::d3n::Actor> itemActor;
        if (rec && rec->snoActor > 0)
            itemActor = renderer.Loader().D3Cache().Actor(rec->snoActor);
        const bool ok =
            rec && chars.SetOutfitItem(*adapter, static_cast<io::d3n::EVisualSlot>(slot), rec, itemActor);
        std::cout << "[dtrace] scenario: equip slot " << slot << " '" << name << "' "
                  << (ok ? (itemActor ? "ok" : "ok (no actor)") : "UNKNOWN ITEM");
        if (slot == 0 && itemActor) {
            // The hair cutaway a helm asks for (tag 0x10404) — the other half of
            // "why is his beard poking through".
            std::cout << " hair="
                      << io::d3n::tagMapValue(itemActor->arTagMap, io::d3n::kTagItemHairStyle).value_or(0);
        }
        std::cout << std::endl;
    }
    // What the outfit resolved to — the child actor and hardpoint per attachment
    // slot. Where "equipped ok but nothing on the model" becomes diagnosable.
    for (const auto& att : chars.OutfitAttachments(*adapter)) {
        std::cout << "[dtrace] scenario: attach slot " << att.visualSlot << " actor " << att.actorSno
                  << " at " << att.hardpoint;
        if (att.visualSlot == 0) {
            // The per-class art actor's own hair tag, beside the item actor's
            // above — which of the two the engine honours is what a wrong beard
            // hinges on.
            if (auto a2 = renderer.Loader().D3Cache().Actor(att.actorSno))
                std::cout << " hair="
                          << io::d3n::tagMapValue(a2->arTagMap, io::d3n::kTagItemHairStyle).value_or(0);
        }
        std::cout << std::endl;
    }
    for (const auto& [slot, dye] : anim.d3Dyes) {
        chars.SetOutfitDye(*adapter, static_cast<io::d3n::EVisualSlot>(slot), dye);
        std::cout << "[dtrace] scenario: dye slot " << slot << " = " << dye << std::endl;
    }
    for (auto* a : spawned)
        renderer.Loader().RestyleD3Model(a->handle);

    // Per-surface material state for the VISIBLE geosets, so "dressed is too
    // bright" becomes a table of pass flags rather than an impression.
    if (anim.probe) {
        if (const auto* st = dynamic_cast<const renderer::profiles::diablo3::D3SurfaceTable*>(
                hero.render.surfaceTable.get())) {
            const auto hidden = adapter->GeosetHidden();
            const auto& surfs = st->Surfaces();
            for (u32 g = 0; g < surfs.size(); ++g) {
                if (g < hidden.size() && hidden[g])
                    continue;
                const auto& s = surfs[g];
                if (!s.valid)
                    continue;
                std::cout << "[dtrace] surf " << g << " fx='" << s.pass.effectFile
                          << "' resolved=" << s.pass.resolved << " lit=" << s.pass.lit
                          << " unlit=" << s.unlit << " vcLights=" << s.pass.vertexColorLights
                          << " gain=" << s.pass.colorGain << " emis=" << s.emissive.x
                          << " chain=" << s.chainCount << std::endl;
            }
        }
    }
#else
    (void)renderer;
    (void)scene;
    (void)spawned;
    (void)anim;
    (void)modelPath;
    (void)contentRoot;
#endif
}

std::optional<i32> AttachAnimations(Actors spawned, std::span<const std::filesystem::path> files) {
    for (const std::filesystem::path& ap : files) {
#if WDX_ENABLE_M3
        std::ifstream in(ap, std::ios::binary);
        std::vector<u8> bytes;
        if (in)
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (bytes.empty()) {
            std::cerr << "[dtrace] --attach-anim unreadable: " << io::PathToUtf8(ap) << std::endl;
            return exit_code::kSpawn;
        }
        // Per adapter, not per actor: the loader may hand every copy the same
        // one, and a second attach of the same label is refused by design.
        std::vector<io::M3ModelAdapter*> done;
        for (auto* a : spawned) {
            if (!a->animation.HasSource())
                continue;
            auto* m3 = dynamic_cast<io::M3ModelAdapter*>(a->animation.Source().get());
            if (!m3)
                continue;
            if (std::find(done.begin(), done.end(), m3) == done.end()) {
                if (!m3->AttachAnimationFile(io::PathToUtf8(ap.stem()), bytes)) {
                    std::cerr << "[dtrace] --attach-anim rejected: " << io::PathToUtf8(ap) << std::endl;
                    return exit_code::kSpawn;
                }
                done.push_back(m3);
            }
            // Bind is what re-reads GetSequences; merged sequences are invisible
            // to playback until it runs.
            a->animation.Bind(a->animation.Source());
        }
        if (done.empty()) {
            std::cerr << "[dtrace] --attach-anim needs an `.m3` model: " << io::PathToUtf8(ap)
                      << std::endl;
            return exit_code::kSpawn;
        }
#else
        (void)ap;
        (void)spawned;
        std::cerr << "[dtrace] --attach-anim needs -DWDX_ENABLE_M3=ON" << std::endl;
        return exit_code::kSpawn;
#endif
    }
    return std::nullopt;
}

namespace {

void PrintSequences(model::Actor& hero, const std::vector<SequenceInfo>& seqs) {
    // The bone count is here because the corpus has to name a model whose palette
    // overflows `kActorPaletteCap`, and that is not guessable from a filename.
    // Through the source rather than the template: `sourceTemplate` is only set
    // for actors born from the template cache.
    if (auto* ms = dynamic_cast<model::IModelSource*>(hero.animation.Source().get()))
        std::cout << "[dtrace] " << ms->GetSkeleton().nodeCount << " bone(s)" << std::endl;
    std::cout << "[dtrace] " << seqs.size() << " sequence(s):" << std::endl;
#if WDX_ENABLE_M3
    auto* m3 = dynamic_cast<io::M3ModelAdapter*>(hero.animation.Source().get());
#endif
    for (usize s = 0; s < seqs.size(); ++s) {
        std::cout << "[dtrace]   [" << s << "] " << seqs[s].name << "  " << seqs[s].startMs << ".."
                  << seqs[s].endMs << "ms" << (seqs[s].nonLooping ? " (non-looping)" : "")
                  << (seqs[s].alwaysPlays ? " (global loop)" : "");
#if WDX_ENABLE_M3
        // The sub-track containers: `subtrack=` in the corpus file names one by
        // index and there is no other way to see the list. Only when there is
        // more than one — a lone `_full` container is what every sequence has.
        if (m3) {
            const auto subs = m3->SubtracksOf(static_cast<i32>(s));
            if (subs.size() > 1) {
                std::cout << "  subtracks:";
                for (usize k = 0; k < subs.size(); ++k)
                    std::cout << " [" << k << "]" << subs[k].name << "(p" << subs[k].priority
                              << (subs[k].concurrent ? ",conc" : ",excl") << "," << subs[k].trackCount
                              << ")";
            }
        }
#endif
        std::cout << std::endl;
    }
}

#if WDX_ENABLE_M3
// The SC2 particle content survey. One line per `PAR_`, straight off the config
// the loader converts — what registration sees, not what the file says, so a
// field the adapter drops shows up as a zero rather than being invisible.
void PrintParticles(renderer::RenderService& renderer, model::Actor& hero, io::M3ModelAdapter& m3p) {
    namespace m3 = ::whiteout::m3;
    static const char* kShape[] = {"Point", "Plane", "Sphere", "Box", "Cyl", "Disc", "Spline", "Mesh"};
    const auto cfgs = m3p.GetSc2ParticleConfigs();
    const auto& src = m3p.SourceModel();

    std::cout << "[dtrace] " << cfgs.size() << " PAR_ emitter(s):" << std::endl;
    for (usize i = 0; i < cfgs.size(); ++i) {
        const auto& c = cfgs[i];
        usize squirtKeys = 0;
        for (const auto& tbl : c.squirt)
            squirtKeys += tbl.size();
        // The desc the LOADER registered, not one this printer converted: the
        // surface index and the priority are stamped by registration. Both id
        // spaces are asked rather than re-deriving which from the flags, and
        // WHICH space answered is itself worth printing.
        using renderer::particle::ParticleOutput;
        auto out = ParticleOutput::Billboard;
        const auto* em = renderer.Particles().GetEmitter(hero.handle, out, static_cast<i32>(i));
        if (!em) {
            out = ParticleOutput::ChildModel;
            em = renderer.Particles().GetEmitter(hero.handle, out, static_cast<i32>(i));
        }
        std::cout << "[dtrace]   [" << i << "] v" << src.particleEmitters[i].getVersion()
                  << " shape=" << (c.emitShape < 8 ? kShape[c.emitShape] : "?")
                  << " vel=" << c.velocityType << " inst=" << int(c.instanceType);
        if (!em) {
            std::cout << " *** NOT REGISTERED ***" << std::endl;
            continue;
        }
        const auto& d = em->AsEmitter2()->Desc();
        std::cout << (d.sc2.motion.analytic ? " analytic" : " euler")
                  << (out == ParticleOutput::ChildModel ? " child" : " quad")
                  << " slots=" << d.sc2.emit.slotBones.size() << " squirt=" << squirtKeys
                  << " surf=" << d.material.m3Surface << " prio=" << d.priorityPlane
                  << " preRoll=" << d.sc2.emit.preRollInit;
        // The peak per container, where the lifetime track is bound; the pre-roll
        // reads the column the active sequence's number names.
        for (usize k = 0; k < d.sc2.emit.preRollPeaks.size(); ++k)
            std::cout << (k == 0 ? "/" : ",") << d.sc2.emit.preRollPeaks[k];
        // Whether the emitter's own flipbook fields are USED is a property of the
        // material's diffuse layer, not of the `PAR_`: a carrier with a sheet and
        // cells authored may still sample cell 0 forever.
        std::cout << (d.sc2.look.flipbookUv ? " flipbookUV" : "")
                  << " cells=" << d.sc2.look.flipbookColumns << "x" << d.sc2.look.flipbookRows;
        // The material's own layers, raw. `uvMapping` 6 is the flipbook; the flag
        // word beside it because 0x100 is documented as "particleUVFlipbook" by
        // the community tools. Printed even when nothing resolves: a material
        // with no DIFFUSE layer must not read like one with no flipbook.
        std::cout << " matm=" << c.materialIndex;
        const auto* mat = io::M3StandardForMaterial(src, c.materialIndex);
        if (!mat) {
            std::cout << " NOT-STANDARD";
        } else {
            if (const auto* l0 = io::M3LayerForSlot(*mat, io::M3LayerSlot::Diffuse)) {
                std::cout << " uvMap=" << static_cast<int>(l0->uvMapping) << " lflags=0x" << std::hex
                          << static_cast<u32>(l0->flags) << std::dec
                          << " cellFrac=" << d.sc2.look.flipbookColumnFraction << "x"
                          << d.sc2.look.flipbookRowFraction << " tex=" << l0->texturePath;
            } else {
                std::cout << " no-diffuse";
            }
            // Which slots the material fills, and which select the flipbook: the
            // axis is per texture SLOT, so an emissive-only material flipbooks
            // if THAT layer says 6.
            std::cout << " slots=";
            for (u32 s = 0; s < static_cast<u32>(io::M3LayerSlot::Count); ++s) {
                const auto* ls = io::M3LayerForSlot(*mat, static_cast<io::M3LayerSlot>(s));
                if (!ls || !io::M3LayerActive(*ls))
                    continue;
                std::cout << s;
                if (ls->uvMapping == m3::UVMappingMode::ParticleFlipbook)
                    std::cout << "*";
                std::cout << ",";
            }
        }
        // What the settle produced: a registered emitter holding nothing is the
        // difference between "the conversion landed" and "the emitter runs".
        std::cout << " alive=" << em->TotalAlive();
        // X6's inputs: the model table a ModelParticles emitter picks from, and
        // the regions a Mesh emitter is born on.
        if (!d.childModelPaths.empty())
            std::cout << " paths=" << d.childModelPaths.size() << "(" << d.childModelPaths.front() << ")";
        if (!d.sc2.emit.shapeRegions.empty())
            std::cout << " regions=" << d.sc2.emit.shapeRegions.size();
        // Only the bits a phase branches on, named.
        const auto has = [&](m3::ParticleFlag f) { return (c.flags & static_cast<u32>(f)) != 0; };
        std::cout << " flags=";
        if (has(m3::ParticleFlag::Sort))
            std::cout << "Sort,";
        if (has(m3::ParticleFlag::CollideTerrain))
            std::cout << "Collide,";
        if (has(m3::ParticleFlag::SpawnTrailingParticles))
            std::cout << "Trail,";
        if (has(m3::ParticleFlag::ModelParticles))
            std::cout << "Model,";
        if (has(m3::ParticleFlag::SimulateInit))
            std::cout << "SimInit,";
        if (has(m3::ParticleFlag::InheritParentVelocity))
            std::cout << "Inherit,";
        if (c.additionalFlags & static_cast<u32>(m3::ParticleAdditionalFlag::WorldSpace))
            std::cout << "World,";
        // What a stretched instance is sized by, and the links X5 routes requests
        // and scale pushes along — a child index that names no emitter only shows
        // here.
        std::cout << " tail=" << c.tailLength;
        if (d.sc2.children.collisionSpawnIndex >= 0)
            std::cout << " cspawn=" << d.sc2.children.collisionSpawnIndex << "("
                      << d.sc2.children.collisionSpawnMin << "-" << d.sc2.children.collisionSpawnMax
                      << "@" << d.sc2.children.collisionSpawnChance
                      << (d.sc2.children.collisionChildIsWorldSpace ? ",world" : ",LOCAL") << ")";
        if (d.sc2.children.trailLinkIndex >= 0)
            std::cout << " trail=" << d.sc2.children.trailLinkIndex << "@" << d.sc2.children.trailChance;
        if ((c.rotationFlags & 0x30u) != 0)
            std::cout << " scalePush=0x" << std::hex << (c.rotationFlags & 0x30u) << std::dec;
        // The squirt keys while there are few enough to read, as
        // `slot/stc:ms=amount@end` — the amount as the SIGNED value the burst sum
        // reads, and the track end a looping player wraps on.
        if (squirtKeys != 0 && squirtKeys <= 8) {
            std::cout << " keys=";
            for (usize s = 0; s < c.squirt.size(); ++s) {
                for (const auto& k : c.squirt[s]) {
                    const auto raw = static_cast<u16>(static_cast<i32>(k.amount));
                    std::cout << s << "/" << k.stc << ":" << k.timeMs << "=" << static_cast<i16>(raw)
                              << "@" << k.trackEnd << ",";
                }
            }
        }
        std::cout << std::endl;
    }
}

// One line per `RIB_`: what the loader converted, whether the service registered
// it, and the fields a Warcraft III crossing sets.
void PrintRibbons(renderer::RenderService& renderer, model::Actor& hero, io::M3ModelAdapter& m3p) {
    const auto cfgs = m3p.GetSc2RibbonConfigs();
    const auto& src = m3p.SourceModel();
    std::cout << "[dtrace] " << cfgs.size() << " RIB_ emitter(s):" << std::endl;
    for (usize i = 0; i < cfgs.size(); ++i) {
        const auto& c = cfgs[i];
        std::string bone = "?";
        if (c.boneIndex >= 0 && static_cast<usize>(c.boneIndex) < src.bones.size()) {
            bone = src.bones[static_cast<usize>(c.boneIndex)].name;
            while (!bone.empty() && bone.back() == '\0')
                bone.pop_back();
        }
        const bool registered = renderer.Ribbons().GetEmitter(hero.handle, static_cast<i32>(i)) != nullptr;
        std::printf("[dtrace]   [%zu] bone=%d '%s' %s type=%u cull=%u divisions=%g "
                    "lifetime=%g gravity=%g drag=%g mass=%g flags=0x%X world=%d matm=%d\n",
                    i, c.boneIndex, bone.c_str(), registered ? "registered" : "NOT REGISTERED",
                    static_cast<unsigned>(c.ribbonType), static_cast<unsigned>(c.cullMethod),
                    c.divisions, c.lifetimeInit, c.gravity3.z, c.drag, c.mass, c.flags,
                    (c.additionalFlags & 0x8u) != 0 ? 1 : 0, c.materialIndex);
    }
    std::fflush(stdout);
}
#endif

} // namespace

bool PrintListing(renderer::RenderService& renderer, model::Actor& hero,
                  const std::vector<SequenceInfo>& seqs, const cli::AnimScenario& anim) {
    if (anim.list) {
        PrintSequences(hero, seqs);
        return true;
    }
#if WDX_ENABLE_M3
    if (anim.particleList || anim.ribbonList) {
        auto* m3p = dynamic_cast<io::M3ModelAdapter*>(hero.animation.Source().get());
        if (anim.particleList) {
            if (m3p)
                PrintParticles(renderer, hero, *m3p);
            else
                std::cout << "[dtrace] not an .m3 — no PAR_ to list" << std::endl;
        } else {
            if (m3p)
                PrintRibbons(renderer, hero, *m3p);
            else
                std::cout << "[dtrace] not an .m3 - no RIB_ to list" << std::endl;
        }
        return true;
    }
#else
    (void)renderer;
#endif
    return false;
}

void ApplyScenario(renderer::RenderService& renderer, Actors spawned,
                   const std::vector<SequenceInfo>& seqs, const cli::AnimScenario& anim) {
    model::Actor& hero = *spawned.front();
    const i32 startSeq = ResolveSequenceSpec(seqs, anim.sequence);
    if (!anim.sequence.empty() && startSeq < 0)
        std::cout << "[dtrace] scenario: no sequence matching '" << anim.sequence << "'" << std::endl;
    if (startSeq >= 0) {
        for (auto* a : spawned)
            a->animation.SetActiveSequenceIndex(startSeq);
        std::cout << "[dtrace] scenario: start seq [" << startSeq << "] "
                  << seqs[static_cast<usize>(startSeq)].name << std::endl;
    }

    // Global loops run unless a scenario says otherwise, because the engine runs
    // them: `M3AnimState::Init` starts every `AlwaysGlobal` sequence before the
    // actor asks for anything. Silencing them is how a baseline isolates one.
    if (anim.noGlobals) {
        for (auto* a : spawned)
            a->animation.Playlist().SetGlobalSequences({});
        std::cout << "[dtrace] scenario: global loops silenced" << std::endl;
    } else {
        const auto globals = std::count_if(seqs.begin(), seqs.end(),
                                           [](const SequenceInfo& s) { return s.alwaysPlays; });
        if (globals)
            std::cout << "[dtrace] scenario: " << globals << " global loop(s) playing" << std::endl;
    }

    // The solver arm. The renderer's default ground is the grid plane at z = 0
    // (physics/ground_plane.h); the harness overrides it with a plane at
    // `groundZ`, because a non-zero height is what makes IK visibly move, and
    // supplies the aim target the renderer has no notion of. Pinned rather than
    // left at the default (on): a golden recorded without the solvers has to
    // keep being captured without them.
    auto& settings = renderer.Settings();
    settings.SetPoseSolversEnabled(anim.solvers);
    if (anim.solvers) {
        const f32 planeZ = anim.groundZ;
        settings.SetGroundQuery([planeZ](const Vector3f& pos, f32 up, f32 down, f32& outZ) {
            if (planeZ > pos.z + up || planeZ < pos.z - down)
                return false;
            outZ = planeZ;
            return true;
        });
        if (anim.hasAim)
            for (auto* a : spawned)
                a->aimTarget = anim.aim;
        // The stage count separates "this model has no solver chunks" from "the
        // solver ran and changed nothing".
        std::cout << "[dtrace] scenario: solvers on, ground z=" << planeZ
                  << (anim.hasAim ? ", aiming" : ", no aim target") << ", "
                  << hero.animation.PoseStages().size() << " stage(s)" << std::endl;
    }

#if WDX_ENABLE_D3
    if (anim.ragdoll) {
        usize armed = 0;
        for (auto* a : spawned) {
            if (auto* d3 = dynamic_cast<io::D3ModelAdapter*>(a->animation.Source().get())) {
                if (d3->HasPhysicsRig()) {
                    d3->SetRagdoll(true);
                    ++armed;
                }
            }
        }
        // The rig count separates "no rig" from "armed and did not move".
        std::cout << "[dtrace] scenario: ragdoll armed on " << armed << " actor(s), "
                  << hero.animation.PoseStages().size() << " stage(s)" << std::endl;
    }
#endif
}

void SelectMeshElements(renderer::RenderService& renderer, Actors spawned, i32 stride) {
    namespace core = renderer::core;
    for (auto* a : spawned) {
        for (const auto& geo : a->render.gpuGeosets) {
            if (!geo.overlaySource)
                continue;
            const auto& src = *geo.overlaySource;
            const auto mark = [&](core::MeshElementKind kind, u32 count) {
                std::vector<u32> selected, hovered;
                for (u32 i = 0; i < count; i += static_cast<u32>(stride)) {
                    selected.push_back(i);
                    hovered.push_back(i + 1);
                }
                renderer.SetMeshElementFlags(a->handle, geo.geosetId, kind, selected,
                                             core::kMeshElementSelected, core::kMeshElementSelected);
                renderer.SetMeshElementFlags(a->handle, geo.geosetId, kind, hovered,
                                             core::kMeshElementHovered, core::kMeshElementHovered);
            };
            mark(core::MeshElementKind::Vertex, static_cast<u32>(src.positions.size()));
            mark(core::MeshElementKind::Face, static_cast<u32>(src.indices.size() / 3));
        }
    }
}

void PrintProbeSetup(model::Actor& hero) {
    // Everything between the sampler and the bound palette, in the order it has
    // to hold. Any `no` here explains a frozen model on its own.
    const auto& sk = hero.render.skinning;
    std::cout << "[dtrace] probe: skeleton=" << (sk.HasSkeleton() ? "yes" : "no")
              << " nodes=" << sk.NodeCount() << " ready=" << (sk.IsReady() ? "yes" : "no")
              << " perActorPalette=" << (sk.UsesPerActorPalette() ? "yes" : "no") << std::endl;
    for (const auto& geo : hero.render.gpuGeosets)
        std::cout << "[dtrace] probe: geoset " << geo.geosetId
                  << " hasSkinning=" << (geo.hasSkinning ? "yes" : "no") << " paletteCb="
                  << (geo.bonePaletteCb != gfx::BufferHandle::Invalid ? "yes" : "no")
                  << " paletteSlots=" << sk.GeosetPaletteSize(geo.geosetId)
                  << " layout=" << geo.layoutId << std::endl;
}

void PrintProbeFrame(model::Actor& hero, i32 frame) {
    // Over the offset matrices rather than the world ones: those are what the
    // palette carries, so a hash that moves here beside a frozen image narrows
    // the fault to the upload or the shader.
    const auto& sk = hero.render.skinning;
    u64 h = 1469598103934665603ull;
    if (const Matrix44f* off = sk.OffsetMatrices()) {
        const auto* raw = reinterpret_cast<const unsigned char*>(off);
        for (usize b = 0; b < sk.NodeCount() * sizeof(Matrix44f); ++b)
            h = (h ^ raw[b]) * 1099511628211ull;
    }
    std::cout << "[dtrace] probe: frame " << frame << " t=" << hero.animation.TimeMs()
              << "ms seq=" << hero.animation.ActiveSequenceIndex()
              << " plays=" << hero.animation.Playlist().PlayCount() << " pose=" << h;
    for (const auto& cl : hero.animation.Playlist().Clips())
        std::cout << " | clip seq=" << cl.sequence << " time=" << cl.timeMs << " elapsed=" << cl.elapsedMs
                  << " w=" << cl.weight;
    std::cout << std::endl;
    // The model's own lights, which a golden cannot speak for: a light inside the
    // silhouette contributes nothing however enabled it is, so whether the
    // visibility gate crossed has to be read off the list itself.
    for (usize li = 0; li < hero.render.activeLights.size(); ++li) {
        const auto& L = hero.render.activeLights[li];
        std::printf("[dtrace] probe: light %zu kind=%d enabled=%d pos=(%.1f %.1f %.1f)"
                    " diffuse=(%.2f %.2f %.2f) atten=%.1f..%.1f\n",
                    li, static_cast<int>(L.kind), L.enabled ? 1 : 0, L.worldPos.x, L.worldPos.y,
                    L.worldPos.z, L.diffuse.x, L.diffuse.y, L.diffuse.z, L.attenStart, L.attenEnd);
    }
    std::fflush(stdout);
}

void PrintAttachments(model::Actor& hero) {
#if WDX_ENABLE_M3
    const auto* m3p = dynamic_cast<const io::M3ModelAdapter*>(hero.animation.Source().get());
    if (!m3p) {
        std::cout << "[dtrace] not an .m3 - no ATT_ to list" << std::endl;
        return;
    }
    const auto& src = m3p->SourceModel();
    const auto nodes = hero.render.skinning.NodeMatrices();
    const auto trim = [](std::string s) {
        while (!s.empty() && s.back() == '\0')
            s.pop_back();
        return s;
    };
    std::cout << "[dtrace] " << src.attachmentPoints.size() << " ATT_:" << std::endl;
    for (usize a = 0; a < src.attachmentPoints.size(); ++a) {
        const auto& point = src.attachmentPoints[a];
        const u32 b = point.boneIndex;
        std::printf("[dtrace]   [%zu] '%s' bone=%u", a, trim(point.name).c_str(), b);
        if (b < src.bones.size()) {
            const auto& bone = src.bones[b];
            std::printf(" '%s' vis=%u%s", trim(bone.name).c_str(), bone.visibility.initValue,
                        bone.visibility.animId != 0 ? " keyed" : "");
        }
        if (b < nodes.size()) {
            const Matrix44f& m = nodes[b];
            std::printf(" at=(%.4f %.4f %.4f)", m.data[3][0], m.data[3][1], m.data[3][2]);
        }
        std::printf("\n");
    }
    std::fflush(stdout);
#else
    (void)hero;
#endif
}

} // namespace whiteout::flakes::harness::draw_trace
