// `.m2` particle emitters: the record -> config -> EmitterDesc path.
//
// Half pure translation checks, half a corpus sweep. The sweep exists because a
// green render-diff on the M2 arm proves nothing on its own about particles —
// if no corpus model carried an emitter, or the configs never reached the
// service, the arm would report ALL MATCH either way. This test is what
// distinguishes "unchanged because correct" from "unchanged because absent",
// and it reports the emitter count so the answer is visible rather than
// inferred.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "io/file_content_provider.h"
#include "io/m2/m2_animation.h"
#include "io/m2/m2_model_adapter.h"
#include "renderer/particle/particle2_emitter.h"
#include "renderer/particle/particle_adapters.h"
#include "whiteout/flakes/content_ref.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace io = whiteout::flakes::io;

using Catch::Approx;
using whiteout::flakes::ContentRef;
using whiteout::flakes::f32;
using whiteout::flakes::usize;
using whiteout::flakes::Vector3f;
using whiteout::flakes::renderer::M2ParticleEmitterConfig;
using namespace whiteout::flakes::renderer::particle;

namespace {

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_WOW_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/WoW");
}

std::vector<fs::path> FindModels() {
    std::vector<fs::path> out;
    std::error_code ec;
    const fs::path root = CorpusRoot();
    if (!fs::is_directory(root, ec))
        return out;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (!it->is_regular_file(ec))
            continue;
        auto ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".m2")
            out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool Finite(f32 v) {
    return v == v && v < 1e30f && v > -1e30f;
}

} // namespace

TEST_CASE("an m2 config translates into a usable emitter desc", "[m2][particle]") {
    M2ParticleEmitterConfig cfg;
    cfg.rows = 2;
    cfg.cols = 4;
    cfg.lifeSpan = 1.5f;
    cfg.lifespanVariation = 0.25f;
    cfg.emissionRateVariation = 3.0f;
    cfg.hasHead = true;
    cfg.hasTail = true;
    cfg.tailLength = 2.0f;
    cfg.generator = M2ParticleEmitterConfig::Generator::Sphere;
    cfg.drag = 0.5f;
    cfg.priorityPlane = 3;
    cfg.colorTimes = {0.0f, 1.0f};
    cfg.colorValues = {{255.0f / 255.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    cfg.alphaTimes = {0.0f, 1.0f};
    cfg.alphaValues = {1.0f, 0.0f};
    cfg.scaleTimes = {0.0f, 1.0f};
    cfg.scaleValues = {{1.0f, 2.0f}, {3.0f, 4.0f}};

    const auto desc = DescFromM2Config(cfg, /*linearColor=*/false);
    REQUIRE(desc);
    REQUIRE(desc->shape);
    CHECK(desc->sheet.rows == 2u);
    CHECK(desc->sheet.cols == 4u);
    CHECK(desc->lifeSpan == Approx(1.5f));
    CHECK(desc->lifespanVariation == Approx(0.25f));
    CHECK(desc->emissionRateVariation == Approx(3.0f));
    CHECK(desc->hasHead);
    CHECK(desc->hasTail);
    CHECK(desc->tailLength == Approx(2.0f));
    CHECK(desc->motion.drag == Approx(0.5f));
    CHECK(desc->priorityPlane == 3);

    // Non-uniform scale survives — the whole reason the size curve is 2D.
    CHECK(desc->curves.size.Evaluate(0.0f).x == Approx(1.0f));
    CHECK(desc->curves.size.Evaluate(0.0f).y == Approx(2.0f));

    // M2 tracks carry no sampling bias: WC3's 0.99/0.005 skew is a WC3 quirk
    // and must not leak into another format's curves.
    CHECK(desc->curves.color.Evaluate(0.0f).x == Approx(1.0f));
    CHECK(desc->curves.alpha.Evaluate(1.0f) == Approx(0.0f));
}

TEST_CASE("m2 colour keys de-gamma only for a linear profile", "[m2][particle]") {
    M2ParticleEmitterConfig cfg;
    cfg.colorTimes = {0.0f};
    cfg.colorValues = {{0.5f, 0.5f, 0.5f}};

    const auto gamma = DescFromM2Config(cfg, /*linearColor=*/false);
    const auto linear = DescFromM2Config(cfg, /*linearColor=*/true);

    CHECK(gamma->curves.color.Evaluate(0.0f).x == Approx(0.5f));
    // sRGB 0.5 is ~0.214 linear. Getting this wrong washes particles out on the
    // HDR profiles and nowhere else, which is hard to spot by eye.
    CHECK(linear->curves.color.Evaluate(0.0f).x == Approx(0.2140f).margin(0.002f));
}

TEST_CASE("each m2 generator picks its own shape", "[m2][particle]") {
    // Not interchangeable: the generators draw different counts of randoms in a
    // different order, which is why they are separate shapes rather than flags.
    for (auto g : {M2ParticleEmitterConfig::Generator::Plane,
                   M2ParticleEmitterConfig::Generator::Sphere,
                   M2ParticleEmitterConfig::Generator::Spline,
                   M2ParticleEmitterConfig::Generator::Bone}) {
        M2ParticleEmitterConfig cfg;
        cfg.generator = g;
        cfg.splinePoints = {{0, 0, 0}, {10, 0, 0}};
        const auto desc = DescFromM2Config(cfg, false);
        REQUIRE(desc->shape);

        // Every shape must produce a finite sample rather than a NaN that only
        // shows up as a vanished particle much later.
        SpawnParams p;
        p.width = 4.0f;
        p.height = 6.0f;
        p.latitude = 0.5f;
        p.horizontalRange = 1.0f;
        p.speed.base = 3.0f;
        RndSeed rnd(12345u);
        SpawnSample s;
        desc->shape->Sample(s, p, rnd);
        CHECK(Finite(s.localPos.x));
        CHECK(Finite(s.localPos.y));
        CHECK(Finite(s.localPos.z));
        CHECK(Finite(s.localVel.x));
        CHECK(Finite(s.localVel.y));
        CHECK(Finite(s.localVel.z));
    }
}

TEST_CASE("zSource aims velocity and skips the angle draws", "[m2][particle]") {
    // The branch is not just a different direction: it consumes two fewer
    // randoms, so everything drawn afterwards shifts with it.
    WowPlaneShape shape;
    SpawnParams p;
    p.width = 0.0f;
    p.height = 0.0f;
    p.latitude = 1.0f;
    p.horizontalRange = 1.0f;
    p.speed.base = 1.0f;
    p.speed.variance = 0.0f;

    SpawnParams aimed = p;
    aimed.zSource = 5.0f;

    RndSeed a(999u), b(999u);
    SpawnSample sa, sb;
    shape.Sample(sa, p, a);
    shape.Sample(sb, aimed, b);

    // Spawn at the origin with zSource above it: the aim is straight down.
    CHECK(sb.localVel.z == Approx(-1.0f).margin(1e-4f));
    // And the two streams have diverged, because the angle draws did not happen.
    CHECK(a.state != b.state);
}

TEST_CASE("the follow factor's line is solved from the record's two samples",
          "[m2][particle]") {
    // The record stores two (speed, scale) points; the runtime wants the line
    // through them. Feeding the raw first pair straight in — the obvious
    // reading of the field names — gives a completely different curve.
    M2ParticleEmitterConfig cfg;
    cfg.followSpeed1 = 2.0f;
    cfg.followScale1 = 0.25f;
    cfg.followSpeed2 = 6.0f;
    cfg.followScale2 = 0.75f;

    const auto desc = DescFromM2Config(cfg, false);
    // slope = (0.75-0.25)/(6-2) = 0.125; bias = 0.25 - 0.125*2 = 0.
    CHECK(desc->followSlope == Approx(0.125f));
    CHECK(desc->followBias == Approx(0.0f).margin(1e-6f));
    // And the line passes through both sample points, which is the whole claim.
    CHECK(desc->followBias + desc->followSlope * 2.0f == Approx(0.25f));
    CHECK(desc->followBias + desc->followSlope * 6.0f == Approx(0.75f));

    // Equal speeds have no line through them: the client clears both terms
    // rather than dividing, leaving the feature inert.
    M2ParticleEmitterConfig flat;
    flat.followSpeed1 = 3.0f;
    flat.followScale1 = 0.9f;
    flat.followSpeed2 = 3.0f;
    flat.followScale2 = 0.1f;
    const auto degenerate = DescFromM2Config(flat, false);
    CHECK(degenerate->followSlope == 0.0f);
    CHECK(degenerate->followBias == 0.0f);
}

TEST_CASE("compressed gravity decodes to a direction and a magnitude",
          "[m2][particle]") {
    // The four bytes are {i8 x, i8 y, i16 mag}: x/y are the direction over 128,
    // Z is whatever is left of the unit vector, and the magnitude's SIGN picks
    // the hemisphere rather than the direction carrying it.
    auto packed = [](whiteout::flakes::i8 x, whiteout::flakes::i8 y,
                     whiteout::flakes::i16 z) {
        whiteout::flakes::u8 raw[4];
        raw[0] = static_cast<whiteout::flakes::u8>(x);
        raw[1] = static_cast<whiteout::flakes::u8>(y);
        std::memcpy(raw + 2, &z, 2);
        f32 out;
        std::memcpy(&out, raw, 4);
        return out;
    };

    // Straight down: no lateral component, negative magnitude flips Z.
    const Vector3f down = io::M2DecodeCompressedGravity(packed(0, 0, -100));
    CHECK(down.x == Approx(0.0f));
    CHECK(down.y == Approx(0.0f));
    CHECK(down.z == Approx(-100.0f * 0.042385526f));

    // Straight up is the same word with the sign cleared — the direction bytes
    // are identical, so a decoder that ignored the sign would give one answer
    // for both.
    const Vector3f up = io::M2DecodeCompressedGravity(packed(0, 0, 100));
    CHECK(up.z == Approx(100.0f * 0.042385526f));

    // Fully lateral: x at +128/128 leaves nothing for Z.
    const Vector3f sideways = io::M2DecodeCompressedGravity(packed(127, 0, 200));
    const f32 mag = 200.0f * 0.042385526f;
    CHECK(sideways.x == Approx((127.0f / 128.0f) * mag));
    CHECK(sideways.z == Approx(std::sqrt(1.0f - (127.0f / 128.0f) * (127.0f / 128.0f)) * mag)
                            .margin(1e-4f));

    // The decoded vector's length is the magnitude, for any direction — that is
    // what makes the two direction bytes a *unit* vector rather than a scale.
    const Vector3f diag = io::M2DecodeCompressedGravity(packed(64, -64, 300));
    const f32 len = std::sqrt(diag.x * diag.x + diag.y * diag.y + diag.z * diag.z);
    CHECK(len == Approx(300.0f * 0.042385526f).margin(1e-3f));
}

TEST_CASE("a plain gravity track is a downward magnitude", "[m2][particle]") {
    // Both forms reach the sim as a vector; only the compressed one carries a
    // direction. Getting this branch backwards points every plain emitter's
    // gravity along a garbage axis, which no unit elsewhere would catch.
    whiteout::m2::AnimationTrack<f32> track;
    track.interpolationType = whiteout::m2::InterpolationType::Linear;
    track.timestamps = {{0u}};
    track.values = {{9.8f}};

    io::M2AnimTime at;
    at.sequence = 0;
    at.timeMs = 0;

    const Vector3f g = io::SampleM2ParticleGravity(track, at, /*compressed=*/false);
    CHECK(g.x == Approx(0.0f));
    CHECK(g.y == Approx(0.0f));
    CHECK(g.z == Approx(-9.8f));
}

TEST_CASE("m2 appearance flags reach the desc", "[m2][particle]") {
    // Named booleans rather than a flag word, because the file bit numbers and
    // the runtime ones do not agree — the loader remaps every one of them
    // (`InitializeLoaded` @0x100f553d0). Carrying the file word through and
    // testing it downstream would bake the wrong numbering in.
    M2ParticleEmitterConfig cfg;
    cfg.velocityOrient = true;
    cfg.inheritBoneScale = true;
    cfg.negateSpinRandom = true;
    cfg.clampTailToAge = true;
    cfg.offsetHeadBySpin = true;
    cfg.unscaledSizeVariation = true;
    cfg.chooseRandomTexture = true;
    cfg.randFlipbookStart = true;
    cfg.baseSpin = 0.5f;
    cfg.baseSpinVariation = 0.1f;
    cfg.spinSpeed = 2.0f;
    cfg.spinSpeedVariation = 0.3f;
    cfg.scaleVariation = {0.2f, 0.4f};

    const auto desc = DescFromM2Config(cfg, false);
    CHECK(desc->velocityOrient);
    CHECK(desc->inheritBoneScale);
    CHECK(desc->negateSpinRandom);
    CHECK(desc->clampTailToAge);
    CHECK(desc->offsetHeadBySpin);
    CHECK(desc->unscaledSizeVariation);
    CHECK(desc->chooseRandomTexture);
    CHECK(desc->randFlipbookStart);
    CHECK(desc->baseSpin == Approx(0.5f));
    CHECK(desc->baseSpinVariation == Approx(0.1f));
    CHECK(desc->spinSpeed == Approx(2.0f));
    CHECK(desc->spinSpeedVariation == Approx(0.3f));
    CHECK(desc->sizeVariation.x == Approx(0.2f));
    CHECK(desc->sizeVariation.y == Approx(0.4f));

    // A default config must leave every one of them inert, or a WC3-shaped
    // emitter borrowed for a test would quietly grow WoW appearance.
    const auto plain = DescFromM2Config(M2ParticleEmitterConfig{}, false);
    CHECK_FALSE(plain->velocityOrient);
    CHECK_FALSE(plain->randFlipbookStart);
    CHECK(plain->spinSpeed == 0.0f);
}

TEST_CASE("twinkle scale is stored as a base and a span", "[m2][particle]") {
    // SetTwinkleScale keeps the record's {min, max} as base and delta, so the
    // per-particle multiplier is uniform in [min, max]. The probe that pinned
    // this fed it {2, 7} and read back 2.0 and 5.0.
    M2ParticleEmitterConfig cfg;
    cfg.twinkleSpeed = 8.0f;
    cfg.twinklePercent = 0.6f;
    cfg.twinkleScale = {2.0f, 7.0f};

    const auto desc = DescFromM2Config(cfg, false);
    CHECK(desc->twinkleSpeed == Approx(8.0f));
    CHECK(desc->twinklePercent == Approx(0.6f));
    CHECK(desc->twinkleBase == Approx(2.0f));
    CHECK(desc->twinkleVary == Approx(5.0f));

    // The common authored case is {1,1}: a multiplier of exactly one, with no
    // spread — twinkle present in the record and inert in effect.
    M2ParticleEmitterConfig unit;
    unit.twinkleScale = {1.0f, 1.0f};
    const auto inert = DescFromM2Config(unit, false);
    CHECK(inert->twinkleBase == Approx(1.0f));
    CHECK(inert->twinkleVary == Approx(0.0f));
}

TEST_CASE("the EXPT/EXP2 extension's zSource beats the record's dead field",
          "[m2][particle]") {
    // The record's zSource track is legacy in any file carrying the extension:
    // it holds the sentinel 255 on ~30.7k corpus emitters, which would aim each
    // of them straight down a virtual source 255 units overhead and discard the
    // authored verticalRange cone. The extension carries the live value.
    //
    // Corpus-backed rather than synthetic because the whole claim is about what
    // shipped data puts in the two fields; a hand-built record could assert the
    // precedence but not that it matters.
    const auto models = FindModels();
    if (models.empty())
        SKIP("no .m2 files under " + CorpusRoot().string());

    io::FileContentProvider provider;
    provider.SetGame(whiteout::flakes::ProductId::Wow);

    struct Expect {
        const char* rel;
        bool wantAimed;   // does the extension ask for a real aim?
        f32 wantZSource;  // what the frame state should carry
    };
    // boundfireelemental: every one of its 24 emitters writes 255 in the record
    // and 0 in the extension, and its flames must rise, not fall.
    // candleboss: one of the 74 emitters that genuinely wants an aim, and it
    // asks for a sane 1/36 of a model unit rather than 255.
    const Expect cases[] = {
        {"creature/boundfireelemental/boundfireelemental.m2", false, 0.0f},
        {"creature/candleboss/candleboss.m2", true, 0.027778f},
    };

    std::size_t checked = 0;
    for (const Expect& e : cases) {
        const fs::path path = CorpusRoot() / e.rel;
        if (!fs::exists(path))
            continue;
        provider.SetBasePath(path.parent_path());
        auto bytes = provider.ReadFile(path.string());
        REQUIRE(bytes.has_value());
        auto adapter = io::M2ModelAdapter::Load(
            ContentRef::FromPath(path.string()),
            std::span<const whiteout::u8>(bytes->data(), bytes->size()), &provider);
        REQUIRE(adapter);

        whiteout::flakes::ClipRef clip{};
        clip.sequence = 0;
        const whiteout::flakes::ClipRef clips[] = {clip};
        whiteout::flakes::PoseRequest req{};
        req.clips = clips;
        const auto state = adapter->Evaluate(req);
        REQUIRE_FALSE(state.particleStates.empty());

        INFO("model " << e.rel);
        bool sawAimed = false;
        for (const auto& ps : state.particleStates) {
            CHECK(Finite(ps.zSource));
            // Never the sentinel: that value reaching a shape is the bug.
            CHECK(ps.zSource < 200.0f);
            if (ps.zSource > 0.001f) {
                sawAimed = true;
                CHECK(ps.zSource == Approx(e.wantZSource).margin(1e-4f));
            }
            ++checked;
        }
        CHECK(sawAimed == e.wantAimed);
    }
    if (checked == 0)
        SKIP("neither reference model is present in this corpus");
}

TEST_CASE("every corpus .m2 particle emitter yields a sane desc", "[m2][particle]") {
    const auto models = FindModels();
    if (models.empty())
        SKIP("no .m2 files under " + CorpusRoot().string());

    io::FileContentProvider provider;
    // Anything Legion or later names its skins by fileDataID, which only a WoW
    // storage can resolve — and the provider defaults to Warcraft III, where
    // those ids mean nothing. Without this the sweep sees a corpus of models
    // that "will not load" when the skins are simply in the install.
    provider.SetGame(whiteout::flakes::ProductId::Wow);
    std::size_t modelsWithEmitters = 0;
    std::size_t emitters = 0;
    std::size_t riding = 0;
    std::size_t zeroTwinkle = 0;
    std::size_t withCells = 0;
    f32 worstLastTime = 0.0f;
    f32 worstAlpha = 0.0f;
    std::string carriers;

    for (const auto& path : models) {
        provider.SetBasePath(path.parent_path());
        const std::string utf8 = path.string();
        INFO("model " << utf8);

        auto bytes = provider.ReadFile(utf8);
        REQUIRE(bytes.has_value());
        auto adapter = io::M2ModelAdapter::Load(
            ContentRef::FromPath(utf8),
            std::span<const whiteout::u8>(bytes->data(), bytes->size()), &provider);
        REQUIRE(adapter);

        const auto configs = adapter->GetM2ParticleConfigs();
        if (configs.empty())
            continue;
        ++modelsWithEmitters;
        emitters += configs.size();
        // Named, not just counted: the render-diff M2 arm can only gate
        // particles if its own corpus list contains one of these.
        carriers += "\n    " + path.filename().string() + " x" + std::to_string(configs.size());

        for (const auto& cfg : configs) {
            INFO("emitter " << (&cfg - configs.data()));
            riding += cfg.modelSpace ? 1 : 0;
            // A {0,0} twinkle range scales every particle to nothing. Faithful
            // if the records really say that, so it is counted rather than
            // clamped — a corpus where most emitters land here would mean the
            // base/span decode is wrong, not that the art is.
            zeroTwinkle +=
                (cfg.twinkleScale.x == 0.0f && cfg.twinkleScale.y == 0.0f) ? 1 : 0;
            withCells += cfg.headCellValues.empty() ? 0 : 1;

            // The fixed16 checks, which only real records can make. Track times
            // are raw/32767 and the last one is asserted to be 1.0 by the
            // client, so a decode off by the 65535 that `unorm16` assumes shows
            // up here as every track ending at 0.5 — and as particles frozen on
            // their first key for the back half of their life.
            for (const f32 t : cfg.alphaTimes)
                CHECK(t <= 1.001f);
            if (!cfg.alphaTimes.empty())
                worstLastTime = (std::max)(worstLastTime, cfg.alphaTimes.back());
            for (const f32 a : cfg.alphaValues) {
                CHECK(a >= 0.0f);
                CHECK(a <= 1.001f);
                worstAlpha = (std::max)(worstAlpha, a);
            }
            // Cell tracks are raw indices, not normalised values: a decode that
            // normalised them would leave every one of these at zero.
            for (const f32 c : cfg.headCellValues)
                CHECK(c == std::floor(c));

            const auto desc = DescFromM2Config(cfg, false);
            REQUIRE(desc);
            REQUIRE(desc->shape);
            CHECK(desc->sheet.rows >= 1u);
            CHECK(desc->sheet.cols >= 1u);
            CHECK(Finite(desc->lifeSpan));
            CHECK(desc->lifeSpan >= 0.0f);
            CHECK(Finite(desc->lifespanVariation));
            CHECK(Finite(desc->motion.drag));
            // An emitter that draws neither head nor tail produces no geometry
            // at all, which is a silent disappearance rather than an error.
            CHECK((desc->hasHead || desc->hasTail));

            // The desc has to actually simulate: a WoW emitter stepped a few
            // frames must produce particles and keep them finite.
            Emitter2 em;
            em.SetDesc(desc);
            em.SetBehavior(ParticleBehavior::Wow());
            em.SetSeed(4242u);
            em.SetEmissionRate(50.0f);
            em.SetLifeSpan(desc->lifeSpan > 0.0f ? desc->lifeSpan : 1.0f);
            em.Spawn().speed.base = 2.0f;
            for (int f = 0; f < 8; ++f) {
                em.SetVisible(true);
                em.Update(1.0f / 30.0f, 1.0f);
            }
            for (usize i = 0; i < em.Pool().AliveCount(); ++i) {
                const Particle2& p = em.Pool()[em.Pool().AliveAt(i)];
                REQUIRE(Finite(p.position.x));
                REQUIRE(Finite(p.position.y));
                REQUIRE(Finite(p.position.z));
                REQUIRE(Finite(p.age));
            }
        }
    }

    WARN("corpus particle emitters: " << emitters << " across " << modelsWithEmitters << " of "
                                      << models.size() << " models; " << riding
                                      << " ride their emitter (file flag 0x10), "
                                      << (emitters - riding) << " are stamped into the world; "
                                      << zeroTwinkle << " have a {0,0} twinkle range, "
                                      << withCells << " animate their sheet cells;"
                                      << " longest alpha track time " << worstLastTime
                                      << ", largest alpha " << worstAlpha << carriers);
    // If this trips, the render-diff M2 arm cannot be telling us anything about
    // particles and its green result must not be read as one.
    CHECK(emitters > 0);
    // The space flag's polarity is decided in the adapter against a real record,
    // so no translation test can reach it. This is the next best thing: over a
    // real sweep both kinds must appear, because an all-or-nothing split is the
    // exact shape a collapsed decode has. Only meaningful in bulk — a corpus
    // narrowed to one model is legitimately all-one-way, so it is not a claim
    // to make there.
    if (emitters >= 20) {
        CHECK(riding > 0);
        CHECK(riding < emitters);
        // The fixed16 scale, which only a real record can settle. Times are
        // asserted by the client to end at exactly 1.0 and alphas are authored
        // up to full; decoding either against 65535 instead of 32767 halves
        // both, and the symptom — a particle stuck on its first key for the
        // back half of its life, at half the intended alpha — reads as "the
        // effect is too dim", not as a decode bug.
        CHECK(worstLastTime > 0.99f);
        CHECK(worstAlpha > 0.99f);
        // And most emitters must want a visible size, or the twinkle range is
        // being read as something it is not.
        CHECK(zeroTwinkle * 2 < emitters);
    }
}
