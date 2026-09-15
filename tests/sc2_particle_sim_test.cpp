// ============================================================================
// SC2 particle load plumbing (SC2_PARTICLE_PLAN.md X2, device-free).
//
// Three kinds of case, mirroring the ribbon's:
//   * conversion — the load-time rules `DescFromSc2ParticleConfig` applies
//     (mid-time clamp, legacy Bezier promotion, the child's space, the pool
//     cap), pinned synthetically. What CanUseGpuMotion decides is not here:
//     that is oracle-gated, vector by vector, in sc2_particle_oracle_test.
//   * surface table — the appended per-`PAR_` block, on a synthetic model,
//     because a material only an emitter references has no geoset row and the
//     whole point is that it gets one anyway.
//   * corpus — one config per parsed `PAR_`, one emission SLOT per `PARC`
//     copy plus the emitter itself, over both corpora. Pointed DIRECTLY at
//     each root: a generic M3 sweep fills its limit from the 33k StarCraft II
//     files and never reaches Heroes.
// ============================================================================

#include "io/m3/m3_model_adapter.h"
#include "m3_anim_builders.h"
#include "renderer/particle/particle2_emitter.h"
#include "renderer/particle/particle_adapters.h"
#include "renderer/particle/particle_geometry.h"
#include "renderer/particle/particle_service.h"
#include "renderer/particle/sc2_model_particle_emitter.h"
#include "renderer/particle/sc2_tick.h"
#include "renderer/profiles/sc2_heroes/m3_surface_table.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace whiteout;
using namespace whiteout::flakes;
using namespace whiteout::flakes::renderer::profiles::sc2_heroes;
namespace particle = whiteout::flakes::renderer::particle;
namespace effects = whiteout::flakes::renderer::effects;
namespace wio = whiteout::flakes::io;
namespace m3 = whiteout::m3;
namespace fs = std::filesystem;

namespace {

fs::path CorpusRoot(const char* leaf) {
    if (const char* v = std::getenv("WDX_TEST_SC2_CORPUS"); v && *v)
        return fs::path(v) / leaf;
    return fs::path("C:/Projects/WhiteoutLib/Corpus") / leaf;
}

std::vector<u8> ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(f), {});
}

std::shared_ptr<particle::EmitterDesc> Convert(const effects::Sc2ParticleEmitterConfig& cfg) {
    return particle::DescFromSc2ParticleConfig(cfg, {});
}

} // namespace

// ---------------------------------------------------------------------------
// Conversion rules
// ---------------------------------------------------------------------------

TEST_CASE("sc2 particle conversion applies Init's load-time rules",
          "[sc2_particle][convert]") {
    effects::Sc2ParticleEmitterConfig cfg;

    SECTION("the family selector is the only thing that says SC2") {
        const auto d = Convert(cfg);
        REQUIRE(d->family == particle::EmitterDesc::Family::Sc2);
        // And a desc nobody converted stays WC3: the selector has one writer,
        // so a Family::Wc3 desc can never carry a filled SC2 block.
        REQUIRE(particle::EmitterDesc{}.family == particle::EmitterDesc::Family::Wc3);
    }

    SECTION("mid times clamp, hold times do not") {
        cfg.midTime[0] = 1.0f;
        cfg.midTime[1] = 0.9959f;
        cfg.midTime[2] = 0.5f;
        cfg.midTime[3] = 2.0f;
        cfg.midHold[0] = 1.0f;
        cfg.flipbookMidTime = 1.0f;
        const auto d = Convert(cfg);
        REQUIRE(d->sc2.look.midTime[0] == Catch::Approx(0.996f));
        REQUIRE(d->sc2.look.midTime[1] == Catch::Approx(0.9959f)); // already under
        REQUIRE(d->sc2.look.midTime[2] == Catch::Approx(0.5f));
        REQUIRE(d->sc2.look.midTime[3] == Catch::Approx(0.996f));
        REQUIRE(d->sc2.look.flipbookMidTime == Catch::Approx(0.996f));
        REQUIRE(d->sc2.look.midHold[0] == Catch::Approx(1.0f));
    }

    SECTION("the legacy Bezier bits leave every smoothing mode as authored") {
        // They convert `Init`'s caches, which the first `UpdateAnimatedParams`
        // re-samples over; the port once promoted them into mode 3, which is
        // not Bezier (2) at all.
        cfg.sizeSmoothing = 0;
        cfg.colorSmoothing = 1;
        cfg.rotationSmoothing = 2;
        cfg.flags = static_cast<u32>(m3::ParticleFlag::OldSizeBezier) |
                    static_cast<u32>(m3::ParticleFlag::OldColorBezier) |
                    static_cast<u32>(m3::ParticleFlag::OldRotationBezier);
        const auto d = Convert(cfg);
        REQUIRE(int(d->sc2.look.sizeSmoothing) == 0);
        REQUIRE(int(d->sc2.look.colorSmoothing) == 1);
        REQUIRE(int(d->sc2.look.rotationSmoothing) == 2);
    }

    SECTION("drag is carried AUTHORED — the 0.01 floor is a spawn-time rule") {
        cfg.drag = 0.0f;
        REQUIRE(Convert(cfg)->sc2.motion.drag == 0.0f);
        cfg.drag = 0.004f;
        REQUIRE(Convert(cfg)->sc2.motion.drag == Catch::Approx(0.004f));
    }

    SECTION("the pool cap is the vertex arena's, not the author's") {
        cfg.maxParticles = 10;
        REQUIRE(Convert(cfg)->sc2.emit.maxParticles == 10u);
        cfg.maxParticles = 1000000;
        REQUIRE(Convert(cfg)->sc2.emit.maxParticles == 0x200000u / 464u);
    }

    SECTION("the shared fields are SET from the block, not duplicated") {
        cfg.flags = static_cast<u32>(m3::ParticleFlag::Sort);
        REQUIRE(Convert(cfg)->sortZ);
        cfg.flags = static_cast<u32>(m3::ParticleFlag::ModelParticles);
        REQUIRE(Convert(cfg)->output == particle::ParticleOutput::ChildModel);
        cfg.flags = 0;
        REQUIRE(Convert(cfg)->output == particle::ParticleOutput::Billboard);
        // World space is the ADDITIONAL flag, and it inverts into modelSpace.
        REQUIRE(Convert(cfg)->modelSpace);
        cfg.additionalFlags = static_cast<u32>(m3::ParticleAdditionalFlag::WorldSpace);
        REQUIRE_FALSE(Convert(cfg)->modelSpace);
        REQUIRE(Convert(cfg)->sc2.emit.worldSpace);
    }

    SECTION("the randomise bits come off additionalFlags, not off the enables") {
        cfg.sizeRandom = true;
        cfg.additionalFlags = static_cast<u32>(m3::ParticleAdditionalFlag::LifespanRandomize);
        const auto d = Convert(cfg);
        REQUIRE(d->sc2.emit.sizeRandom);
        REQUIRE(d->sc2.emit.lifetimeRandom);
        REQUIRE_FALSE(d->sc2.emit.speedRandom);
        REQUIRE_FALSE(d->sc2.emit.massRandom);
    }

    SECTION("+0x5D0 is a non-zero TEST, not a probability") {
        cfg.modelOrientPreset = 0.0f;
        REQUIRE_FALSE(Convert(cfg)->sc2.children.modelOrientLegacy);
        cfg.modelOrientPreset = 0.25f;
        REQUIRE(Convert(cfg)->sc2.children.modelOrientLegacy);
    }

    SECTION("the collision child's SPACE is resolved at load, not per bounce") {
        std::vector<effects::Sc2ParticleEmitterConfig> pair(2);
        pair[1].additionalFlags = static_cast<u32>(m3::ParticleAdditionalFlag::WorldSpace);
        pair[0].collisionSpawnIndex = 1;
        REQUIRE(particle::DescFromSc2ParticleConfig(pair[0], pair)
                    ->sc2.children.collisionChildIsWorldSpace);
        // An index nothing answers falls back rather than dropping the link —
        // whether the child exists is the loader's business, not this one's.
        pair[0].collisionSpawnIndex = 7;
        REQUIRE_FALSE(particle::DescFromSc2ParticleConfig(pair[0], pair)
                          ->sc2.children.collisionChildIsWorldSpace);
        REQUIRE(particle::DescFromSc2ParticleConfig(pair[0], pair)
                    ->sc2.children.collisionSpawnIndex == 7);
    }
}

TEST_CASE("an SC2 desc survives SetDesc - it has no IParticleShape",
          "[sc2_particle][convert]") {
    // The regression this exists for: `SetDesc` replaced any desc whose
    // `shape` was null with the WC3 default, and an SC2 desc's emission shape
    // is a NUMBER, not an `IParticleShape`. Every SC2 emitter therefore
    // registered as an inert WC3 emitter — no crash, no red gate, and every
    // byte-identical render arm still byte-identical, because what landed was
    // an emitter that emits nothing.
    effects::Sc2ParticleEmitterConfig cfg;
    cfg.emitShape = static_cast<u8>(m3::EmitterShape::Sphere);
    cfg.instanceType = 4;
    cfg.slotBones.push_back(7);
    cfg.squirt.emplace_back();
    auto desc = Convert(cfg);
    REQUIRE(desc->shape == nullptr); // the premise: SC2 descs carry no shape

    particle::Emitter2 em;
    em.SetDesc(desc);
    REQUIRE(em.Desc().family == particle::EmitterDesc::Family::Sc2);
    REQUIRE(em.Desc().sc2.look.instanceType == 4);
    REQUIRE(em.Desc().sc2.emit.slotBones.size() == 1);
    REQUIRE(em.Desc().sc2.emit.shape == static_cast<u8>(m3::EmitterShape::Sphere));

    // And the WC3 invariant the guard exists for still holds: a shapeless desc
    // of any other family is still replaced, because CreateParticle would
    // dereference that null.
    auto wc3 = std::make_shared<particle::EmitterDesc>();
    REQUIRE(wc3->family == particle::EmitterDesc::Family::Wc3);
    em.SetDesc(wc3);
    REQUIRE(em.Desc().shape != nullptr);
}

// ---------------------------------------------------------------------------
// The appended per-`PAR_` surface block
// ---------------------------------------------------------------------------

TEST_CASE("a material only a PAR_ references still gets a surface row",
          "[sc2_particle][surface]") {
    // Two materials, one geoset. The geoset draws material 0; material 1 is
    // named only by the emitter, so without the appended block it has no row
    // and the emitter's draw has no surface to resolve to.
    m3::Model model;
    model.materialMaps.resize(2);
    model.divisions.resize(1);
    model.divisions[0].regions.resize(1);
    model.particleEmitters.resize(2);
    model.particleEmitters[0].materialIndex = 1;
    model.particleEmitters[1].materialIndex = 0;

    const std::size_t regions[1] = {0};
    const u32 materials[1] = {0};
    auto table = BuildM3SurfaceTable(model, regions, materials);
    REQUIRE(table != nullptr);

    const i32 base = table->ParticleSurfaceBase();
    REQUIRE(base >= 0);
    // One row per record, after the geoset rows — and after the ribbon block
    // when there is one, which is what keeps the two id spaces independent.
    REQUIRE(table->Count() == static_cast<usize>(base) + 2);
    REQUIRE(table->Surface(static_cast<u32>(base)) != nullptr);
    REQUIRE(table->Surface(static_cast<u32>(base) + 1) != nullptr);
    // No ribbons here, so that base stays absent rather than aliasing this one.
    REQUIRE(table->RibbonSurfaceBase() == -1);
}

TEST_CASE("the ribbon and particle surface blocks do not overlap",
          "[sc2_particle][surface]") {
    m3::Model model;
    model.materialMaps.resize(1);
    model.divisions.resize(1);
    model.divisions[0].regions.resize(1);
    model.ribbonEmitters.resize(3);
    model.particleEmitters.resize(2);

    const std::size_t regions[1] = {0};
    const u32 materials[1] = {0};
    auto table = BuildM3SurfaceTable(model, regions, materials);
    REQUIRE(table->RibbonSurfaceBase() >= 0);
    REQUIRE(table->ParticleSurfaceBase() == table->RibbonSurfaceBase() + 3);
    REQUIRE(table->Count() == static_cast<usize>(table->ParticleSurfaceBase()) + 2);
}

// ---------------------------------------------------------------------------
// Corpus registration
// ---------------------------------------------------------------------------

namespace {

struct SweepTotals {
    std::size_t models = 0, records = 0, copies = 0, meshShape = 0, splineShape = 0;
    // Two counts, not one, because they are the pair that catches a broken
    // flatten: `squirtBound` is how many `squirtAmount` refs name a track at
    // all, `squirtKeys` how many keys came back. Bound with no keys means the
    // walk over the containers found nothing, which is invisible from a total.
    std::size_t squirtBound = 0, squirtKeys = 0;
    std::size_t modelParticles = 0;
    // The flipbook axis is per texture SLOT (`b_iUVMapping[slot]`), and the
    // loader reads slot 0. These say what that costs: `fbNoDiffuse` is how
    // many emitters have no ACTIVE diffuse layer at all — a slot-0 read is
    // blind on every one of them — and `fbMixed` how many have active layers
    // that DISAGREE, which is the case no single baked UV set can serve.
    std::size_t fbSlot0 = 0, fbAny = 0, fbNoDiffuse = 0, fbMixed = 0;
    // A count is not a carrier. X6 needs one model of each exotic shape to put
    // in `corpus_m3_particle.txt`, and "145 mesh emitters exist" does not name
    // one — the same gap the ModelParticles example already closed.
    std::string modelParticleExample, meshExample, splineExample, fbMixedExample;
    // X4 needs a carrier that COLLIDES without waiting on X5: a rate-driven
    // emitter (no squirt keys) on the Euler path, drawn as a billboard.
    std::string collideExample;
};

void SweepCorpus(const fs::path& root, SweepTotals& t, std::size_t limit) {
    if (!fs::exists(root))
        return;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (t.models >= limit)
            return;
        if (!entry.is_regular_file() || entry.path().extension() != ".m3")
            continue;

        // Carriers are found by CONTENT: the chunk tag, reversed on disk, is
        // the cheap pre-filter and the parse decides.
        const auto bytes = ReadAll(entry.path());
        static constexpr char kTag[] = {'_', 'R', 'A', 'P'};
        if (std::search(bytes.begin(), bytes.end(), std::begin(kTag), std::end(kTag)) ==
            bytes.end())
            continue;

        const std::string name = entry.path().filename().string();
        auto adapter =
            wio::M3ModelAdapter::Load(ContentRef::FromPath(entry.path().string()), bytes);
        if (!adapter)
            continue;
        const auto& src = adapter->SourceModel();
        if (src.particleEmitters.empty())
            continue;
        ++t.models;

        const auto configs = adapter->GetSc2ParticleConfigs();
        INFO(name);
        // One config per parsed record, in order — the emitter id IS the index.
        REQUIRE(configs.size() == src.particleEmitters.size());

        for (std::size_t i = 0; i < configs.size(); ++i) {
            const auto& c = configs[i];
            ++t.records;
            INFO(name << " PAR_ " << i);

            // Slots: the emitter itself plus one per `PARC` copy it names. The
            // squirt tables are index-parallel, and slot 0's bone IS the
            // emitter's — a copy list read as an emitter list would make these
            // disagree immediately.
            REQUIRE(c.slotBones.size() == c.squirt.size());
            REQUIRE(c.slotBones.size() >= 1);
            REQUIRE(c.slotBones[0] == static_cast<i32>(src.particleEmitters[i].boneIndex));
            std::size_t valid = 0;
            for (const u32 ci : src.particleEmitters[i].copyIndices)
                valid += (ci < src.particleEmitterCopies.size()) ? 1 : 0;
            REQUIRE(c.slotBones.size() == valid + 1);
            t.copies += valid;

            // Which SLOTS the material fills, and which of them select the
            // flipbook. Counted off the raw material rather than the resolved
            // surface so this measures the file, not our resolution of it.
            if (const auto* mat = wio::M3StandardForMaterial(src, static_cast<u32>(c.materialIndex))) {
                bool anyFb = false, anyPlain = false, diffuseActive = false;
                for (u32 s = 0; s < static_cast<u32>(wio::M3LayerSlot::Count); ++s) {
                    const auto* l = wio::M3LayerForSlot(*mat, static_cast<wio::M3LayerSlot>(s));
                    if (!l || !wio::M3LayerActive(*l))
                        continue;
                    const bool fb = l->uvMapping == ::whiteout::m3::UVMappingMode::ParticleFlipbook;
                    if (s == static_cast<u32>(wio::M3LayerSlot::Diffuse)) {
                        diffuseActive = true;
                        if (fb)
                            ++t.fbSlot0;
                    }
                    anyFb |= fb;
                    anyPlain |= !fb;
                }
                if (anyFb)
                    ++t.fbAny;
                if (!diffuseActive)
                    ++t.fbNoDiffuse;
                if (anyFb && anyPlain) {
                    ++t.fbMixed;
                    if (t.fbMixedExample.empty())
                        t.fbMixedExample = name + " PAR_" + std::to_string(i);
                }
            }

            if (t.collideExample.empty() && c.instanceType == 0 &&
                (c.flags & static_cast<u32>(::whiteout::m3::ParticleFlag::CollideTerrain)) != 0) {
                std::size_t keys = 0;
                for (const auto& tbl : c.squirt)
                    keys += tbl.size();
                if (keys == 0)
                    t.collideExample = name + " PAR_" + std::to_string(i);
            }

            const u32 squirtAnim = src.particleEmitters[i].squirtAmount.animId;
            if (squirtAnim != 0 && squirtAnim != 0xFFFFFFFFu)
                ++t.squirtBound;
            for (const auto& tbl : c.squirt)
                t.squirtKeys += tbl.size();

            // Value ranges that would catch a mis-mapped field. The shape is
            // the one the RE settled — 6 Spline, 7 Mesh — and a swap would put
            // a spline's control points on a mesh emitter.
            REQUIRE(c.emitShape <= 7);
            if (c.emitShape == static_cast<u8>(m3::EmitterShape::Mesh)) {
                ++t.meshShape;
                if (t.meshExample.empty())
                    t.meshExample = name;
                // A mesh emitter that names no region has nothing to be born
                // on; the shipped data does not do that.
                REQUIRE_FALSE(c.shapeRegions.empty());
            }
            if (c.emitShape == static_cast<u8>(m3::EmitterShape::Spline)) {
                ++t.splineShape;
                if (t.splineExample.empty())
                    t.splineExample = name;
                REQUIRE_FALSE(src.particleEmitters[i].splineLineData.empty());
            }
            if (c.flags & static_cast<u32>(m3::ParticleFlag::ModelParticles)) {
                ++t.modelParticles;
                if (t.modelParticleExample.empty())
                    t.modelParticleExample = name;
                // RE §16.16: the model index is an unguarded `div` by the
                // table size, so a ModelParticles emitter that authored no
                // path FAULTS in retail. Shipped data should never do it, and
                // if it does the port needs a guard retail lacks.
                REQUIRE_FALSE(c.modelPaths.empty());
            }
            REQUIRE(c.instanceType <= 10);
            // A pre-roll peak per container, or none for an unbound track; the
            // raw init value is retail's and may be negative.
            for (const f32 peak : c.preRollPeaks)
                REQUIRE(std::isfinite(peak));
            REQUIRE(std::isfinite(c.preRollInit));

            // And the conversion survives every one of them.
            const auto desc = particle::DescFromSc2ParticleConfig(c, configs);
            REQUIRE(desc->family == particle::EmitterDesc::Family::Sc2);
            REQUIRE(desc->sc2.emit.slotBones.size() == c.slotBones.size());
            REQUIRE(desc->sc2.look.midTime[0] <= 0.996f);
        }
    }
}

} // namespace

TEST_CASE("the pre-roll peaks are the gated answer for each container",
          "[sc2_particle][convert][preroll]") {
    // RE §16.33. `EmitBurst` reads ONE column of the curve table, seeded at
    // zero, or the raw init value where that column has no track. The load path
    // once seeded every column with the init value and took the largest key
    // over all of them; a finiteness check passes both readings, this does not.
    constexpr u32 kLife = 500, kLifeRandom = 501;
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);
    m3fix::StcBuilder low("low");
    low.Float(kLife, m3fix::Block<f32>({0, 1000}, {0.25f, 0.5f}));
    low.Float(kLifeRandom, m3fix::Block<f32>({0, 1000}, {-1.0f, -0.5f}));
    m3fix::StcBuilder high("high");
    high.Float(kLife, m3fix::Block<f32>({0, 1000}, {3.0f, 1.0f}));
    m3fix::StcBuilder bare("bare");
    bare.Float(999, m3fix::Block<f32>({0, 1000}, {7.0f, 7.0f}));
    const u32 lowIdx = mb.AddStc(low.Build());
    const u32 highIdx = mb.AddStc(high.Build());
    const u32 bareIdx = mb.AddStc(bare.Build());
    mb.Sequence("Low", 0, 1000, {lowIdx});
    mb.Sequence("High", 0, 1000, {highIdx});
    mb.Sequence("Bare", 0, 1000, {bareIdx});
    m3::Model model = mb.Build();

    m3::ParticleEmitter par;
    par.lifetime = m3fix::Ref<f32>(kLife, 2.0f, 1, 6);
    par.lifetimeRandom = m3fix::Ref<f32>(kLifeRandom, 4.0f, 1, 6);

    SECTION("the lifetime track, one column per container") {
        model.particleEmitters = {par};
        wio::M3ModelAdapter adapter(model);
        const auto configs = adapter.GetSc2ParticleConfigs();
        REQUIRE(configs.size() == 1u);
        const auto& c = configs[0];
        REQUIRE(c.preRollPeaks.size() == 3u);
        CHECK(c.preRollPeaks[0] == 0.5f); // keys below the init value: seeded at zero
        CHECK(c.preRollPeaks[1] == 3.0f); // this container's own keys
        CHECK(c.preRollPeaks[2] == 2.0f); // no track here: the raw init value
        CHECK(c.preRollInit == 2.0f);
    }

    SECTION("the random track under LifespanRandomize, where a curve below zero reads zero") {
        par.additionalFlags = static_cast<m3::ParticleAdditionalFlag>(
            static_cast<u32>(m3::ParticleAdditionalFlag::LifespanRandomize));
        model.particleEmitters = {par};
        wio::M3ModelAdapter adapter(model);
        const auto configs = adapter.GetSc2ParticleConfigs();
        REQUIRE(configs.size() == 1u);
        const auto& c = configs[0];
        REQUIRE(c.preRollPeaks.size() == 3u);
        CHECK(c.preRollPeaks[0] == 0.0f);
        CHECK(c.preRollPeaks[1] == 4.0f);
        CHECK(c.preRollPeaks[2] == 4.0f);
        CHECK(c.preRollInit == 4.0f);
    }
}

TEST_CASE("the parent-scale push reaches the child's transform on a shipped model",
          "[sc2_particle][corpus][children]") {
    // RE §16.34 on the one StarCraft II carrier where it shows:
    // MiraHorner_NapalmBomb_Coop_Explosion's PAR_6 pushes onto its trail child
    // PAR_9. The parent's bone rests at 0.758 and the child's at 1, so the
    // child's rows come out at the parent's lengths with its position kept, and
    // the parent, which nothing pushes onto, keeps its own bone.
    const fs::path path = CorpusRoot("Sc2M3") / "MiraHorner_NapalmBomb_Coop_Explosion.m3";
    if (!fs::exists(path))
        SKIP("no " + path.string());
    const auto bytes = ReadAll(path);
    const auto adapter = wio::M3ModelAdapter::Load(ContentRef::FromPath(path.string()), bytes);
    REQUIRE(adapter);
    const auto& src = adapter->SourceModel();
    REQUIRE(src.particleEmitters.size() > 9u);
    REQUIRE((static_cast<u32>(src.particleEmitters[6].rotationFlags) & 0x20u) != 0u);
    REQUIRE(src.particleEmitters[6].trailLinkIndex == 9);

    const auto bind = adapter->Evaluate(PoseRequest{});
    const auto stateOf = [&bind](i32 id) {
        for (const auto& st : bind.particleStates)
            if (st.emitterId == id)
                return &st;
        return static_cast<decltype(&bind.particleStates[0])>(nullptr);
    };
    const auto* child = stateOf(9);
    const auto* parent = stateOf(6);
    REQUIRE(child != nullptr);
    REQUIRE(parent != nullptr);

    const std::size_t childBone = src.particleEmitters[9].boneIndex;
    const std::size_t parentBone = src.particleEmitters[6].boneIndex;
    REQUIRE(childBone < bind.boneWorldMatrices.size());
    REQUIRE(parentBone < bind.boneWorldMatrices.size());
    const Matrix44f& own = bind.boneWorldMatrices[childBone];
    const Matrix44f& pusher = bind.boneWorldMatrices[parentBone];
    const auto rowLength = [](const Matrix44f& m, std::size_t r) {
        return std::sqrt(m.data[r][0] * m.data[r][0] + m.data[r][1] * m.data[r][1] +
                         m.data[r][2] * m.data[r][2]);
    };
    // Otherwise the case proves nothing: the two bones really do differ.
    REQUIRE(rowLength(pusher, 0) == Catch::Approx(0.758f).epsilon(1e-3));
    REQUIRE(rowLength(own, 0) == Catch::Approx(1.0f).epsilon(1e-3));

    for (std::size_t r = 0; r < 3; ++r) {
        INFO("row " << r);
        CHECK(rowLength(child->transform, r) == Catch::Approx(rowLength(pusher, r)).epsilon(1e-4));
    }
    for (std::size_t k = 0; k < 3; ++k)
        CHECK(child->transform.data[3][k] == Catch::Approx(own.data[3][k]).margin(1e-4));
    for (std::size_t r = 0; r < 4; ++r)
        for (std::size_t k = 0; k < 4; ++k)
            CHECK(parent->transform.data[r][k] == pusher.data[r][k]);
}

TEST_CASE("the parent-scale push keeps the scale of the child bone's parent",
          "[sc2_particle][convert][children]") {
    // The shipped case is two root bones, where a bone's model-space rows ARE
    // its local scale. Under a scaled parent they are not: the push replaces the
    // child's LOCAL scale and its parent's scale still applies on top (RE
    // §16.34). A pusher at 3 onto a child under a parent at 2 gives rows of 6; a
    // port that took the child's model-space rows as its local scale gives 3.
    m3fix::ModelBuilder mb;
    const auto bone = [&mb](const char* name, i32 parent, Vector3f pos, f32 scale) {
        mb.Bone(name, parent, m3fix::ConstRef(pos), m3fix::ConstRef(Quaternion{0, 0, 0, 1}),
                m3fix::ConstRef(Vector3f{scale, scale, scale}), m3::BoneFlag::None);
    };
    bone("scaled", -1, {0, 0, 0}, 2.0f);
    bone("child", 0, {1, 0, 0}, 1.0f);
    bone("pusher", -1, {0, 0, 5}, 3.0f);
    m3::Model model = mb.Build();

    m3::ParticleEmitter pusher;
    pusher.boneIndex = 2;
    pusher.rotationFlags = static_cast<decltype(pusher.rotationFlags)>(0x20u);
    pusher.trailLinkIndex = 1;
    m3::ParticleEmitter child;
    child.boneIndex = 1;
    model.particleEmitters = {pusher, child};

    const wio::M3ModelAdapter adapter(model);
    const auto bind = adapter.Evaluate(PoseRequest{});
    REQUIRE(bind.particleStates.size() == 2u);
    REQUIRE(bind.boneWorldMatrices.size() == 3u);
    const auto rowLength = [](const Matrix44f& m, std::size_t r) {
        return std::sqrt(m.data[r][0] * m.data[r][0] + m.data[r][1] * m.data[r][1] +
                         m.data[r][2] * m.data[r][2]);
    };
    const Matrix44f& own = bind.boneWorldMatrices[1];
    REQUIRE(rowLength(own, 0) == Catch::Approx(2.0f));
    for (const auto& st : bind.particleStates) {
        if (st.emitterId == 1) {
            for (std::size_t r = 0; r < 3; ++r) {
                INFO("row " << r);
                CHECK(rowLength(st.transform, r) == Catch::Approx(6.0f));
            }
            for (std::size_t k = 0; k < 3; ++k)
                CHECK(st.transform.data[3][k] == Catch::Approx(own.data[3][k]).margin(1e-5));
        } else {
            CHECK(rowLength(st.transform, 0) == Catch::Approx(3.0f));
        }
    }
}

TEST_CASE("every corpus PAR_ registers with one slot per copy",
          "[sc2_particle][corpus]") {
    // 64 models per root. The budget now buys FEWER `PAR_` records than it used
    // to (6361 assertions against 6624) and that is not a regression: an
    // effect-only `.m3` — no `REGN`, one emitter, one bone — used to be refused
    // by `M3ModelAdapter::Load` outright, and 1291 of the StarCraft II root's
    // files are that shape. They load now, they sort early, and each carries
    // one emitter where a unit carries six. `WDX_TEST_M3_LIMIT` buys depth when
    // it is wanted: 300 reaches 1652 records, all green.
    std::size_t limit = 64;
    if (const char* v = std::getenv("WDX_TEST_M3_LIMIT"); v && *v)
        limit = static_cast<std::size_t>(std::strtoul(v, nullptr, 10));

    SweepTotals sc2, hots;
    // Each root gets its own budget. Sharing one would spend it all on the 33k
    // StarCraft II files and never open a Heroes model.
    SweepCorpus(CorpusRoot("Sc2M3"), sc2, limit);
    SweepCorpus(CorpusRoot("HotSM3"), hots, limit);

    if (sc2.models == 0 && hots.models == 0)
        SKIP("no SC2/HotS corpus with PAR_ carriers");

    WARN("Sc2M3: " << sc2.models << " carriers, " << sc2.records << " PAR_, " << sc2.copies
                   << " PARC, " << sc2.meshShape << " mesh, " << sc2.splineShape << " spline, "
                   << sc2.squirtBound << " squirt-bound / " << sc2.squirtKeys << " keys"
                   << " | HotSM3: " << hots.models << " carriers, " << hots.records << " PAR_, "
                   << hots.copies << " PARC, " << hots.meshShape << " mesh, " << hots.splineShape
                   << " spline, " << hots.squirtBound << " squirt-bound / " << hots.squirtKeys
                   << " keys"
                   << " | ModelParticles: " << sc2.modelParticles + hots.modelParticles << " (e.g. "
                   << (sc2.modelParticleExample.empty() ? hots.modelParticleExample
                                                        : sc2.modelParticleExample)
                   << ") | mesh e.g. "
                   << (sc2.meshExample.empty() ? hots.meshExample : sc2.meshExample)
                   << " | spline e.g. "
                   << (sc2.splineExample.empty() ? hots.splineExample : sc2.splineExample)
                   << " | flipbook slot0 " << sc2.fbSlot0 + hots.fbSlot0 << " / any slot "
                   << sc2.fbAny + hots.fbAny << ", no active diffuse "
                   << sc2.fbNoDiffuse + hots.fbNoDiffuse << ", slots disagree "
                   << sc2.fbMixed + hots.fbMixed << " (e.g. "
                   << (sc2.fbMixedExample.empty() ? hots.fbMixedExample : sc2.fbMixedExample)
                   << ") | rate-driven collider e.g. "
                   << (sc2.collideExample.empty() ? hots.collideExample : sc2.collideExample));
    // A bound track that yields no keys is a broken flatten, not empty data.
    if (sc2.squirtBound + hots.squirtBound > 0)
        REQUIRE(sc2.squirtKeys + hots.squirtKeys > 0);
    REQUIRE(sc2.records + hots.records > 0);
}

TEST_CASE("the named corpus carriers register the versions the plan names",
          "[sc2_particle][corpus]") {
    // The four the plan calls out by name, because each covers a version the
    // generic sweep may or may not reach: Zealot/Ultralisk are v12 (the
    // migrated randomise dwords), BattleCruiser carries `PARC` copies, SCV is
    // v24.
    const char* kNamed[] = {"Zealot.m3", "Ultralisk.m3", "BattleCruiser.m3", "SCV.m3"};
    const fs::path root = CorpusRoot("Sc2M3");
    if (!fs::exists(root))
        SKIP("no corpus at " + root.string());

    std::size_t seen = 0;
    for (const char* leaf : kNamed) {
        const fs::path p = root / leaf;
        if (!fs::exists(p))
            continue;
        const auto bytes = ReadAll(p);
        auto adapter = wio::M3ModelAdapter::Load(ContentRef::FromPath(p.string()), bytes);
        REQUIRE(adapter != nullptr);
        ++seen;
        const auto configs = adapter->GetSc2ParticleConfigs();
        INFO(leaf << ": " << configs.size() << " PAR_");
        REQUIRE(configs.size() == adapter->SourceModel().particleEmitters.size());
        for (const auto& c : configs) {
            REQUIRE(c.slotBones.size() == c.squirt.size());
            REQUIRE_FALSE(c.slotBones.empty());
            const auto desc = particle::DescFromSc2ParticleConfig(c, configs);
            REQUIRE(desc->family == particle::EmitterDesc::Family::Sc2);
        }
    }
    if (seen == 0)
        SKIP("none of the named carriers are in this corpus");
}

TEST_CASE("the X6 carrier's model particles name .m3 paths that load",
          "[sc2_particle][corpus][model]") {
    // A `.m3` string reference counts its terminator. A table path that kept
    // it never ended in ".m3", so the loader's `.m3` route declined every
    // birth and each child came back null — with every service case green,
    // because those author their paths by hand. The sweep above never reaches
    // this carrier inside its default budget, so it is named here.
    const fs::path root = CorpusRoot("Sc2M3");
    const fs::path carrier = root / "Artillery_Mengsk_COOP.m3";
    if (!fs::exists(carrier))
        SKIP("no carrier at " + carrier.string());
    const auto bytes = ReadAll(carrier);
    auto adapter = wio::M3ModelAdapter::Load(ContentRef::FromPath(carrier.string()), bytes);
    REQUIRE(adapter != nullptr);

    std::size_t tables = 0;
    for (const auto& c : adapter->GetSc2ParticleConfigs()) {
        if (!(c.flags & static_cast<u32>(m3::ParticleFlag::ModelParticles)))
            continue;
        ++tables;
        const auto desc = particle::DescFromSc2ParticleConfig(c, {});
        REQUIRE(desc->childModelPaths.size() == c.modelPaths.size());
        for (const std::string& path : desc->childModelPaths) {
            INFO(path);
            REQUIRE(path.find('\0') == std::string::npos);
            REQUIRE(path.ends_with(".m3"));
            // The corpus is flat, so the child resolves by its leaf.
            const fs::path leaf = root / fs::path(path).filename();
            if (!fs::exists(leaf))
                continue;
            const auto child = ReadAll(leaf);
            CHECK(wio::M3ModelAdapter::Load(ContentRef::FromPath(leaf.string()), child) != nullptr);
        }
    }
    CHECK(tables == 2u);
}

// ---------------------------------------------------------------------------
// Children through the service (SC2_PARTICLE_PLAN.md X5)
// ---------------------------------------------------------------------------

namespace {

/// A frame state for one SC2 emitter of the test model. Value-initialised:
/// the WC3 half of the struct is plain PODs, and a zero `transform` would put
/// every particle through a collapsed matrix.
renderer::model::FrameState::ParticleFrameState Sc2State(i32 id, f32 rate) {
    renderer::model::FrameState::ParticleFrameState st{};
    st.emitterId = id;
    st.transform = Matrix44f::identity();
    st.visibility = 1.0f;
    st.unitScale = 1.0f;
    st.sc2.active = true;
    st.sc2.emissionRate = rate;
    st.sc2.lifetime = 5.0f;
    st.sc2.lifetimeRandom = 5.0f;
    st.sc2.size3 = {1.0f, 1.0f, 1.0f};
    for (auto& c : st.sc2.colorBGRA)
        c = 0xFFFFFFFFu;
    return st;
}

/// A falling emitter whose particles spawn two or three children each where
/// they hit the ground, and the world-space child that takes the requests.
std::vector<effects::Sc2ParticleEmitterConfig> CollisionPair() {
    effects::Sc2ParticleEmitterConfig parent;
    parent.flags = static_cast<u32>(m3::ParticleFlag::CollideTerrain);
    parent.maxParticles = 256;
    parent.gravity3 = {0.0f, 0.0f, -9.8f};
    parent.collisionSpawnIndex = 1;
    parent.collisionSpawnMin = 2;
    parent.collisionSpawnMax = 4; // hi-exclusive: two or three
    parent.collisionSpawnChance = 1.0f;
    parent.collisionSpawnEnergy = 1.0f;
    parent.slotBones = {0};
    parent.squirt.emplace_back();

    effects::Sc2ParticleEmitterConfig child;
    child.additionalFlags = static_cast<u32>(m3::ParticleAdditionalFlag::WorldSpace);
    child.maxParticles = 1024;
    child.slotBones = {0};
    child.squirt.emplace_back();
    return {parent, child};
}

} // namespace

TEST_CASE("a collision spawn reaches its child through the service",
          "[sc2_particle][service]") {
    // The one join no kernel gate can see: MOVE queues onto the parent's
    // outbox, the service routes by the child's index, and the child
    // materialises the requests on its next EMIT. The parent is registered
    // first, so its requests land on the child in the same frame.
    const auto configs = CollisionPair();
    particle::ParticleService service;
    service.SetGroundQuery([](const Vector3f& p, f32 up, f32 down, f32& outZ) {
        constexpr f32 kGround = -1.0f;
        if (kGround > p.z + up || kGround < p.z - down)
            return false;
        outZ = kGround;
        return true;
    });
    for (i32 i = 0; i < 2; ++i) {
        auto em = std::make_unique<particle::Emitter2>();
        em->SetDesc(particle::DescFromSc2ParticleConfig(configs[static_cast<usize>(i)], configs));
        service.AddEmitter(1, i, std::move(em));
    }
    particle::Emitter2* parent =
        service.GetEmitter(1, particle::ParticleOutput::Billboard, 0)->AsEmitter2();
    particle::Emitter2* child =
        service.GetEmitter(1, particle::ParticleOutput::Billboard, 1)->AsEmitter2();
    REQUIRE(parent != nullptr);
    REQUIRE(child != nullptr);
    REQUIRE(parent->Desc().sc2.children.collisionChildIsWorldSpace);

    const auto run = [&](int frames, f32 parentRate) {
        for (int k = 0; k < frames; ++k) {
            parent->ApplyState(Sc2State(0, parentRate));
            child->ApplyState(Sc2State(1, 0.0f));
            service.Simulate(1.0f / 60.0f);
        }
    };

    SECTION("particles that land spawn children, and die doing it") {
        // Half a second before anything can fall a unit: nothing yet.
        run(20, 30.0f);
        CHECK(child->TotalAlive() == 0);
        // Two seconds of falling and landing.
        run(120, 30.0f);
        const i32 children = child->TotalAlive();
        CHECK(children > 0);
        // Every landing parent dies and leaves two or three behind it, so the
        // children outnumber the parents that are still in the air.
        CHECK(children >= 2 * (static_cast<i32>(parent->Desc().sc2.emit.maxParticles) / 16));
        CHECK(parent->TotalAlive() < 30 * 2);
    }

    SECTION("a reset empties the queues as well as the pools") {
        run(140, 30.0f);
        REQUIRE(child->TotalAlive() > 0);
        service.ResetEmitters();
        CHECK(parent->TotalAlive() == 0);
        CHECK(child->TotalAlive() == 0);
        // With the parent silent nothing is owed to the child any more.
        run(30, 0.0f);
        CHECK(child->TotalAlive() == 0);
    }
}

TEST_CASE("a collision spawn reaches a model-particle child in the ChildModel id space",
          "[sc2_particle][service][model]") {
    // A child registers under the output it draws, so a ModelParticles child
    // answers to ChildModel and nothing answers index 1 in the billboard
    // space. The service asks both; a router that asked only the billboard
    // space would drop every request here and birth nothing.
    auto configs = CollisionPair();
    configs[1].flags = static_cast<u32>(m3::ParticleFlag::ModelParticles);
    configs[1].modelPaths = {"a.m3"};

    particle::ParticleService service;
    service.SetGroundQuery([](const Vector3f& p, f32 up, f32 down, f32& outZ) {
        constexpr f32 kGround = -1.0f;
        if (kGround > p.z + up || kGround < p.z - down)
            return false;
        outZ = kGround;
        return true;
    });
    auto parentEm = std::make_unique<particle::Emitter2>();
    parentEm->SetDesc(particle::DescFromSc2ParticleConfig(configs[0], configs));
    particle::Emitter2* parent = parentEm.get();
    service.AddEmitter(1, 0, std::move(parentEm));

    u32 nextHandle = 1;
    auto childEm = std::make_unique<particle::Sc2ModelParticleEmitter>(
        1u, 1, [&nextHandle] { return nextHandle++; });
    childEm->SetDesc(particle::DescFromSc2ParticleConfig(configs[1], configs));
    REQUIRE(childEm->Desc().output == particle::ParticleOutput::ChildModel);
    particle::Emitter2* child = childEm.get();
    service.AddEmitter(1, 1, std::move(childEm));
    REQUIRE(service.GetEmitter(1, particle::ParticleOutput::Billboard, 1) == nullptr);

    std::vector<particle::ChildModelEvent> events;
    i32 births = 0;
    for (int k = 0; k < 140; ++k) {
        parent->ApplyState(Sc2State(0, 30.0f));
        child->ApplyState(Sc2State(1, 0.0f));
        service.Simulate(1.0f / 60.0f);
        service.DrainChildModelEvents(events);
        for (const auto& ev : events)
            births += ev.kind == particle::ChildModelEvent::Kind::Birth ? 1 : 0;
        events.clear();
    }
    INFO("births=" << births << " child alive=" << child->TotalAlive());
    // The child's own rate is zero: everything it holds came in as a request.
    CHECK(child->TotalAlive() > 0);
    CHECK(births > 0);
}

TEST_CASE("a burst queued before an emitter's first tick is not dropped",
          "[sc2_particle][service]") {
    // The actor layer's first crossing arrives ahead of the first Simulate.
    // An emitter that only sized its slots on its first tick dropped that
    // burst: `QueueBurst` refuses a slot that does not exist yet, which is
    // exactly how a squirt on a sequence's first frame went missing.
    effects::Sc2ParticleEmitterConfig cfg;
    cfg.maxParticles = 64;
    cfg.slotBones = {0};
    cfg.squirt.emplace_back();
    particle::Emitter2 em;
    em.SetDesc(particle::DescFromSc2ParticleConfig(cfg, {}));
    em.QueueBurst(0, 5);
    em.ApplyState(Sc2State(0, 0.0f));
    em.Update(1.0f / 60.0f, 1.0f);
    CHECK(em.TotalAlive() == 5);
    // And spent with the frame: nothing more without a new burst.
    em.ApplyState(Sc2State(0, 0.0f));
    em.Update(1.0f / 60.0f, 1.0f);
    CHECK(em.TotalAlive() == 5);
}

TEST_CASE("an SC2 burst spawns its count whatever the actor's world scale",
          "[sc2_particle][service]") {
    // A frame state's `unitScale` is the renderer's world scale — 100 for an
    // SC2 actor — and the tick once passed it as `ComputeEmitCount`'s element
    // scale, so a squirt key of 1 filled the pool. Every case above runs at a
    // unit scale of 1, where the two readings agree.
    effects::Sc2ParticleEmitterConfig cfg;
    cfg.maxParticles = 64;
    cfg.slotBones = {0};
    cfg.squirt.emplace_back();
    particle::Emitter2 em;
    em.SetDesc(particle::DescFromSc2ParticleConfig(cfg, {}));
    auto st = Sc2State(0, 0.0f);
    st.unitScale = 100.0f;
    em.QueueBurst(0, 3);
    em.ApplyState(st);
    em.Update(1.0f / 60.0f, 1.0f);
    CHECK(em.TotalAlive() == 3);
}

TEST_CASE("an SC2 emitter at world scale 100 builds 100x its scale-1 geometry",
          "[sc2_particle][service][units]") {
    // The runtime runs in SC2 units and the actor's world scale is a boundary
    // the emitter converts across, so the same emitter at 100 renderer units
    // per SC2 unit has to build exactly its scale-1 quads, 100 times as big.
    // Any term fed renderer units where retail's constant assumes game units
    // breaks that: world-space gravity and kill radius, a Tail quad's
    // `|v|·tail`, the noise offset.
    enum Extra : u32 { kNone = 0, kGround = 1, kSlot = 2 };
    const auto build = [](const effects::Sc2ParticleEmitterConfig& cfg, f32 unit, u32 extra) {
        particle::Emitter2 em;
        em.SetDesc(particle::DescFromSc2ParticleConfig(cfg, {}));
        if (extra & kGround) {
            // A floor half an SC2 unit below the emitter, answered in the
            // host's units as a scene's ground query answers.
            em.SetGroundQuery([unit](const Vector3f& p, f32 up, f32 down, f32& outZ) {
                const f32 floor = -0.5f * unit;
                if (floor > p.z + up || floor < p.z - down)
                    return false;
                outZ = floor;
                return true;
            });
        }
        // A camera 30 SC2 units up z looking down it, and the emitter off the
        // origin — both in the host's units.
        Matrix44f view = Matrix44f::identity();
        view.data[3][2] = -30.0f * unit;
        auto st = Sc2State(0, 40.0f);
        st.unitScale = unit;
        for (usize k = 0; k < 3; ++k)
            st.transform.data[k][k] = unit;
        st.transform.data[3][0] = 0.5f * unit;
        st.transform.data[3][1] = -0.25f * unit;
        st.worldPosition = {0.5f * unit, -0.25f * unit, 0.0f};
        st.sc2.speed = 3.0f;
        st.sc2.lifetime = 1.5f;
        st.sc2.lifetimeRandom = 1.5f;
        st.sc2.size3 = {0.2f, 0.2f, 0.2f};
        if (extra & kSlot) {
            // A `PARC` copy on a bone two units out and one up, in host units
            // as the adapter samples it.
            Matrix44f bone = Matrix44f::identity();
            for (usize k = 0; k < 3; ++k)
                bone.data[k][k] = unit;
            bone.data[3][0] = 2.0f * unit;
            bone.data[3][2] = 1.0f * unit;
            st.sc2.slots.push_back({30.0f, bone});
        }
        for (int frame = 0; frame < 60; ++frame) {
            em.ApplyState(st);
            em.SetSc2Scene(view, unit);
            em.Update(1.0f / 60.0f, 1.0f);
        }
        std::vector<renderer::Vertex> verts;
        particle::BuildGeometryInput in;
        in.worldToView = &view;
        em.BuildGeometry(in, verts);
        return verts;
    };
    const auto sameShape = [&](const effects::Sc2ParticleEmitterConfig& cfg, u32 extra = kNone) {
        const auto one = build(cfg, 1.0f, extra);
        const auto hundred = build(cfg, 100.0f, extra);
        REQUIRE(!one.empty());
        REQUIRE(hundred.size() == one.size());
        usize off = 0;
        f32 worst = 0.0f;
        const auto lane = [&](f32 small, f32 big) {
            // A non-finite lane is a mismatch: NaN compares false both ways,
            // so it would otherwise pass here and vanish from every min.
            if (!std::isfinite(small) || !std::isfinite(big)) {
                ++off;
                return;
            }
            const f32 want = small * 100.0f;
            const f32 err = std::fabs(big - want) / (std::max)(1.0f, std::fabs(want));
            worst = (std::max)(worst, err);
            off += err > 1e-4f ? 1u : 0u;
        };
        for (usize i = 0; i < one.size(); ++i) {
            lane(one[i].position.x, hundred[i].position.x);
            lane(one[i].position.y, hundred[i].position.y);
            lane(one[i].position.z, hundred[i].position.z);
        }
        INFO("worst relative error " << worst << " over " << one.size() << " vertices");
        CHECK(off == 0u);
    };
    const auto tail = [] {
        effects::Sc2ParticleEmitterConfig cfg;
        cfg.maxParticles = 256;
        cfg.instanceType = 1; // Tail
        cfg.tailLength = 1.5f;
        cfg.slotBones = {0};
        cfg.squirt.emplace_back();
        return cfg;
    };

    SECTION("a world-space Tail on the closed form, under gravity") {
        auto cfg = tail();
        cfg.additionalFlags = static_cast<u32>(m3::ParticleAdditionalFlag::WorldSpace);
        cfg.gravity3 = {0.0f, 0.0f, -9.8f};
        sameShape(cfg);
    }

    SECTION("a world-space Tail on the CPU path, with a kill radius") {
        auto cfg = tail();
        cfg.additionalFlags = static_cast<u32>(m3::ParticleAdditionalFlag::WorldSpace);
        cfg.gravity3 = {0.0f, 0.0f, -9.8f};
        cfg.killRadius = 4.0f; // demotes to the CPU step
        sameShape(cfg);
    }

    SECTION("a local-space Tail with noise") {
        auto cfg = tail();
        cfg.noiseAmplitude = 0.5f; // demotes to the CPU step
        cfg.noiseFrequency = 2.0f;
        cfg.noiseCoherence = 1.0f;
        cfg.noiseEdge = 0.25f;
        sameShape(cfg);
    }

    SECTION("a world-space emitter that lands on the host's ground") {
        // A billboard, not a Tail: a Tail at rest has no velocity to lay its
        // quad along and builds NaN corners, which pass every comparison
        // here. A ground asked in the wrong units hid behind exactly that.
        auto cfg = tail();
        cfg.instanceType = 0;
        cfg.flags = static_cast<u32>(m3::ParticleFlag::CollideTerrain);
        cfg.additionalFlags = static_cast<u32>(m3::ParticleAdditionalFlag::WorldSpace);
        cfg.gravity3 = {0.0f, 0.0f, -9.8f};
        sameShape(cfg, kGround);
        // And the floor was really struck: a second of falling puts a particle
        // that never touched it well below -1, where a landed one rests half a
        // unit down with a quad a few tenths tall. Without this the comparison
        // above holds just as well when nothing collides at either scale — and
        // without the floorless run it holds when nothing FALLS.
        usize nonFinite = 0;
        const auto lowestOf = [&](u32 extra) {
            f32 lowest = 0.0f;
            for (const auto& v : build(cfg, 1.0f, extra)) {
                nonFinite += std::isfinite(v.position.z) ? 0u : 1u;
                lowest = (std::min)(lowest, v.position.z);
            }
            return lowest;
        };
        const f32 fell = lowestOf(kNone);
        const f32 landed = lowestOf(kGround);
        INFO("lowest vertex z: " << fell << " with no floor, " << landed << " on it; "
             << nonFinite << " non-finite vertices");
        CHECK(nonFinite == 0u);
        CHECK(fell < -1.5f);
        CHECK(landed > -1.2f);
    }

    SECTION("a PARC copy emitting from its own bone") {
        auto cfg = tail();
        cfg.slotBones = {0, 1};
        cfg.squirt.emplace_back();
        sameShape(cfg, kSlot);
    }
}

TEST_CASE("SC2 model particles balance Birth against Death, each with a row of its table",
          "[sc2_particle][service][model]") {
    // The PE1/M2 invariant, through the SC2 route: an unmatched Birth is a
    // leaked actor, an unmatched Death one destroyed twice. What is SC2's own
    // is that the birth comes out of the frame's pending walk and names which
    // of the three models this particle became.
    effects::Sc2ParticleEmitterConfig cfg;
    cfg.flags = static_cast<u32>(m3::ParticleFlag::ModelParticles);
    cfg.maxParticles = 64;
    cfg.slotBones = {0};
    cfg.squirt.emplace_back();
    cfg.modelPaths = {"a.m3", "b.m3", "c.m3"};

    particle::ParticleService service;
    u32 nextHandle = 1;
    auto em = std::make_unique<particle::Sc2ModelParticleEmitter>(
        1u, 0, [&nextHandle] { return nextHandle++; });
    em->SetDesc(particle::DescFromSc2ParticleConfig(cfg, {}));
    REQUIRE(em->Desc().output == particle::ParticleOutput::ChildModel);
    particle::Emitter2* raw = em.get();
    service.AddEmitter(1, 0, std::move(em));

    std::set<u32> live;
    std::set<u32> everBorn;
    std::set<u32> rows;
    i32 births = 0, deaths = 0, transforms = 0;
    std::vector<particle::ChildModelEvent> events;
    for (int frame = 0; frame < 240; ++frame) {
        auto st = Sc2State(0, 30.0f);
        st.sc2.lifetime = 0.5f;
        st.sc2.lifetimeRandom = 0.5f;
        raw->ApplyState(st);
        service.Simulate(1.0f / 60.0f);
        service.DrainChildModelEvents(events);
        for (const auto& ev : events) {
            REQUIRE(ev.childHandle != 0u);
            switch (ev.kind) {
            case particle::ChildModelEvent::Kind::Birth:
                REQUIRE(everBorn.insert(ev.childHandle).second);
                REQUIRE(live.insert(ev.childHandle).second);
                REQUIRE(ev.pathIndex < 3u);
                rows.insert(ev.pathIndex);
                ++births;
                break;
            case particle::ChildModelEvent::Kind::Death:
                REQUIRE(live.erase(ev.childHandle) == 1u);
                ++deaths;
                break;
            case particle::ChildModelEvent::Kind::Transform:
                REQUIRE(live.count(ev.childHandle) == 1u);
                ++transforms;
                break;
            }
        }
        events.clear();
        // Every particle alive after a frame was reached by that frame's walk.
        REQUIRE(static_cast<i32>(live.size()) == raw->TotalAlive());
    }
    INFO("births=" << births << " deaths=" << deaths << " transforms=" << transforms);
    CHECK(births > 0);
    CHECK(deaths > 0);
    CHECK(transforms > 0);
    CHECK(births - deaths == static_cast<i32>(live.size()));
    // A uniform pick over three rows, a hundred-odd times.
    CHECK(rows.size() == 3u);

    SECTION("a reset reports every child it held and starts clean") {
        const std::size_t held = live.size();
        REQUIRE(held > 0u);
        service.ResetEmitters();
        service.DrainChildModelEvents(events);
        std::size_t reported = 0;
        for (const auto& ev : events)
            reported += ev.kind == particle::ChildModelEvent::Kind::Death ? 1u : 0u;
        CHECK(reported == held);
        CHECK(raw->TotalAlive() == 0);
    }
}

namespace {

/// A ModelParticles emitter on a service, and the events it has reported.
struct ModelRig {
    particle::ParticleService service;
    u32 nextHandle = 1;
    particle::Emitter2* emitter = nullptr;
    std::set<u32> live;
    std::set<u32> everBorn;
    i32 births = 0;
    i32 deaths = 0;
    std::vector<particle::ChildModelEvent> last;

    explicit ModelRig(u32 maxParticles, u32 instanceType = 0) {
        effects::Sc2ParticleEmitterConfig cfg;
        cfg.flags = static_cast<u32>(m3::ParticleFlag::ModelParticles);
        cfg.maxParticles = maxParticles;
        cfg.instanceType = static_cast<u8>(instanceType);
        cfg.slotBones = {0};
        cfg.squirt.emplace_back();
        cfg.modelPaths = {"a.m3", "b.m3", "c.m3"};
        auto em = std::make_unique<particle::Sc2ModelParticleEmitter>(
            1u, 0, [this] { return nextHandle++; });
        em->SetDesc(particle::DescFromSc2ParticleConfig(cfg, {}));
        emitter = em.get();
        service.AddEmitter(1, 0, std::move(em));
    }

    /// One frame; every event is checked against the protocol as it drains.
    void Step(const renderer::model::FrameState::ParticleFrameState& st,
              f32 frameDt = 1.0f / 60.0f) {
        emitter->ApplyState(st);
        service.Simulate(frameDt);
        last.clear();
        service.DrainChildModelEvents(last);
        for (const auto& ev : last) {
            REQUIRE(ev.childHandle != 0u);
            if (ev.kind == particle::ChildModelEvent::Kind::Birth) {
                REQUIRE(everBorn.insert(ev.childHandle).second);
                REQUIRE(live.insert(ev.childHandle).second);
                REQUIRE(ev.pathIndex < 3u);
                ++births;
            } else if (ev.kind == particle::ChildModelEvent::Kind::Death) {
                REQUIRE(live.erase(ev.childHandle) == 1u);
                ++deaths;
            } else {
                REQUIRE(live.count(ev.childHandle) == 1u);
            }
        }
    }
};

} // namespace

TEST_CASE("an SC2 model particle that dies before the walk gets no model and leaves no entry",
          "[sc2_particle][service][model]") {
    // What makes the kill's unregistering observable: a particle dies in one
    // sub-step's MOVE, a LATER sub-step of the same frame takes its slot, and
    // that occupant lives to the walk. An entry the kill left behind would hand
    // it a second birth, and the first child would never see a death.
    //
    // The sub-step rate follows the frame, so a 60 Hz frame takes one step:
    // every spawn precedes every kill and a stale entry is only ever skipped.
    // Hence 210 ms frames — three 15 Hz steps — and two spawns a step into four
    // slots, because a refused spawn crowds the rest of the batch's birth times
    // to the frame's start and nothing would live to the walk. The lives sweep
    // one to three steps so the pattern does not hang on a kill boundary.
    for (int lifeMs = 70; lifeMs <= 200; lifeMs += 10) {
        ModelRig rig(4);
        for (int frame = 0; frame < 60; ++frame) {
            auto st = Sc2State(0, 30.0f);
            st.sc2.lifetime = static_cast<f32>(lifeMs) * 0.001f;
            st.sc2.lifetimeRandom = st.sc2.lifetime;
            rig.Step(st, 0.21f);
            INFO("life " << lifeMs << " ms, frame " << frame << " births=" << rig.births
                         << " deaths=" << rig.deaths);
            REQUIRE(static_cast<i32>(rig.live.size()) <= rig.emitter->TotalAlive());
        }
        INFO("life " << lifeMs << " ms");
        CHECK(rig.births > 0);
        CHECK(rig.births - rig.deaths == static_cast<i32>(rig.live.size()));
    }
}

TEST_CASE("an SC2 model particle's transform follows its particle",
          "[sc2_particle][service][model]") {
    // `SimulateParticles` re-poses a model at every sub-step. A pose written
    // only at birth would leave every child where its particle was born.
    ModelRig rig(64);
    auto st = Sc2State(0, 30.0f);
    st.sc2.speed = 60.0f; // straight up at rest
    u32 first = 0;
    f32 bornZ = 0.0f;
    f32 laterZ = 0.0f;
    for (int frame = 0; frame < 90 && laterZ == 0.0f; ++frame) {
        rig.Step(st);
        for (const auto& ev : rig.last) {
            if (first == 0u && ev.kind == particle::ChildModelEvent::Kind::Birth) {
                first = ev.childHandle;
                bornZ = ev.transform.data[3][2];
            } else if (first != 0u && frame > 30 && ev.childHandle == first &&
                       ev.kind == particle::ChildModelEvent::Kind::Transform) {
                laterZ = ev.transform.data[3][2];
            }
        }
    }
    REQUIRE(first != 0u);
    INFO("born at z=" << bornZ << ", later at z=" << laterZ);
    CHECK(laterZ > bornZ + 10.0f);
}

TEST_CASE("an SC2 Bezier size channel poses its model through the authored mid key",
          "[sc2_particle][service][model][bezier]") {
    // `ApplyState` turns a Bezier channel's sampled keys into the curve's
    // control point, as `UpdateAnimatedParams` does before anything reads
    // them. A symmetric curve whose mid key is 4 then peaks near 4 at the mid
    // time, as the linear curve does; fed the raw keys, the Bezier would pass
    // through 2.5 there instead.
    const auto scaleAtMidLife = [](u8 smoothing) {
        effects::Sc2ParticleEmitterConfig cfg;
        cfg.flags = static_cast<u32>(m3::ParticleFlag::ModelParticles);
        cfg.maxParticles = 8;
        cfg.slotBones = {0};
        cfg.squirt.emplace_back();
        cfg.modelPaths = {"a.m3"};
        cfg.sizeSmoothing = smoothing;
        cfg.midTime[0] = 0.5f;
        particle::ParticleService service;
        u32 nextHandle = 1;
        auto em = std::make_unique<particle::Sc2ModelParticleEmitter>(
            1u, 0, [&nextHandle] { return nextHandle++; });
        em->SetDesc(particle::DescFromSc2ParticleConfig(cfg, {}));
        particle::Emitter2* raw = em.get();
        service.AddEmitter(1, 0, std::move(em));
        std::vector<particle::ChildModelEvent> events;
        u32 first = 0;
        int bornFrame = -1;
        f32 scale = 0.0f;
        for (int frame = 0; frame < 120 && scale == 0.0f; ++frame) {
            // A few particles up front and none after, so the first one ages
            // undisturbed through its one-second life.
            auto st = Sc2State(0, frame < 3 ? 60.0f : 0.0f);
            st.sc2.size3 = {1.0f, 4.0f, 1.0f};
            st.sc2.lifetime = 1.0f;
            st.sc2.lifetimeRandom = 1.0f;
            raw->ApplyState(st);
            service.Simulate(1.0f / 60.0f);
            events.clear();
            service.DrainChildModelEvents(events);
            for (const auto& ev : events) {
                if (first == 0u && ev.kind == particle::ChildModelEvent::Kind::Birth) {
                    first = ev.childHandle;
                    bornFrame = frame;
                } else if (first != 0u && ev.childHandle == first && frame == bornFrame + 30 &&
                           ev.kind == particle::ChildModelEvent::Kind::Transform) {
                    const auto& r = ev.transform.data[0];
                    scale = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
                }
            }
        }
        return scale;
    };
    const f32 linear = scaleAtMidLife(0);
    const f32 bezier = scaleAtMidLife(2);
    INFO("mid-life model scale: linear " << linear << ", Bezier " << bezier);
    REQUIRE(linear > 0.0f);
    // Half a frame either side of the mid time the linear peak's kink costs a
    // few percent; the unconverted Bezier would sit 37% below it.
    CHECK(bezier == Catch::Approx(linear).epsilon(0.05));
}

TEST_CASE("an SC2 camera-facing model particle is born on the scene camera's rows",
          "[sc2_particle][service][model]") {
    // What `SetSc2Scene` hands the pose: right, view direction and up, in that
    // order. The compose case pins the kernel's answer for given rows; this
    // pins which rows the emitter gives it.
    ModelRig rig(64, 0);
    const f32 c = std::cos(0.7f), s = std::sin(0.7f);
    const f32 cp = std::cos(0.3f), sp = std::sin(0.3f);
    Matrix44f view = Matrix44f::identity();
    view.data[0] = {c, s, 0.0f, 0.0f};
    view.data[1] = {-s * cp, c * cp, -sp, 0.0f};
    view.data[2] = {-s * sp, c * sp, cp, 0.0f};
    rig.emitter->SetSc2Scene(view, 1.0f);
    const particle::Sc2QuadCamera cam = particle::Sc2CameraFromView(view);

    bool checked = false;
    for (int frame = 0; frame < 30 && !checked; ++frame) {
        rig.Step(Sc2State(0, 30.0f));
        for (const auto& ev : rig.last) {
            if (ev.kind != particle::ChildModelEvent::Kind::Birth)
                continue;
            const auto near = [&](i32 row, const Vector3f& want) {
                return std::fabs(ev.transform.data[row][0] - want.x) < 1e-4f &&
                       std::fabs(ev.transform.data[row][1] - want.y) < 1e-4f &&
                       std::fabs(ev.transform.data[row][2] - want.z) < 1e-4f;
            };
            CHECK(near(0, cam.billboardRight));
            CHECK(near(1, cam.direction));
            CHECK(near(2, cam.billboardUp));
            checked = true;
            break;
        }
    }
    CHECK(checked);
}

TEST_CASE("an SC2 Mesh emitter is born only on the regions it names",
          "[sc2_particle][service][mesh]") {
    // The emitter's triangle table is built from `shapeRegions` when the mesh
    // is set. Two regions five and fifty units up; the emitter names the
    // second, so every quad it builds is up there.
    effects::Sc2ParticleEmitterConfig cfg;
    cfg.emitShape = 7;
    cfg.maxParticles = 256;
    cfg.slotBones = {0};
    cfg.squirt.emplace_back();
    cfg.shapeRegions = {1};
    auto mesh = std::make_shared<particle::EmitMesh>();
    mesh->subs = {particle::EmitMesh::SubMesh{0u, 1u}, particle::EmitMesh::SubMesh{1u, 1u}};
    mesh->tris = {0u, 1u, 2u, 3u, 4u, 5u};
    mesh->rest = {Vector3f{0.0f, 0.0f, 5.0f},  Vector3f{2.0f, 0.0f, 5.0f},
                  Vector3f{0.0f, 2.0f, 5.0f},  Vector3f{0.0f, 0.0f, 50.0f},
                  Vector3f{2.0f, 0.0f, 50.0f}, Vector3f{0.0f, 2.0f, 50.0f}};

    particle::ParticleService service;
    auto em = std::make_unique<particle::Emitter2>();
    em->SetDesc(particle::DescFromSc2ParticleConfig(cfg, {}));
    em->SetEmitMesh(mesh);
    particle::Emitter2* raw = em.get();
    service.AddEmitter(1, 0, std::move(em));
    for (int frame = 0; frame < 30; ++frame) {
        raw->ApplyState(Sc2State(0, 120.0f));
        service.Simulate(1.0f / 60.0f);
    }
    REQUIRE(raw->TotalAlive() > 0);

    std::vector<renderer::Vertex> verts;
    std::vector<particle::EmitterDrawList> draws;
    service.BuildGeometry(Matrix44f::identity(), verts, draws);
    REQUIRE_FALSE(verts.empty());
    for (const auto& v : verts) {
        INFO("vertex at z=" << v.position.z);
        CHECK(v.position.z > 40.0f);
    }
}

TEST_CASE("a pending model particle that dies first is unregistered by swap-remove",
          "[sc2_particle][model]") {
    // RE §16.16: retail overwrites the dead entry with the LAST one. A stable
    // erase would keep the survivors' order and hand every element after the
    // death a different model from the one retail gives it.
    particle::Sc2Runtime a;
    particle::Sc2Runtime b;
    particle::Sc2PendingModels list{{&a, 0}, {&a, 1}, {&b, 0}, {&a, 2}};
    particle::Sc2SwapRemovePending(list, &a, 0);
    REQUIRE(list.size() == 3u);
    CHECK((list[0].runtime == &a && list[0].node == 2));
    CHECK((list[1].runtime == &a && list[1].node == 1));
    CHECK((list[2].runtime == &b && list[2].node == 0));
    // The node alone does not identify an entry: another emitter's node 1 is
    // not this one's.
    particle::Sc2SwapRemovePending(list, &b, 1);
    CHECK(list.size() == 3u);
    particle::Sc2SwapRemovePending(list, &b, 0);
    REQUIRE(list.size() == 2u);
    CHECK((list[0].runtime == &a && list[0].node == 2));
    CHECK((list[1].runtime == &a && list[1].node == 1));
}
