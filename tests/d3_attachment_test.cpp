// ============================================================================
// Diablo III keyframed attachments — the anim route, end to end.
//
// `d3_effect_test` gates the *data*: the census, the payload/type pairing, and
// what the resolver does with one TriggerEvent. This gates the *playback* —
// `D3ModelAdapter::ClipAttachments` turning frames into milliseconds, and
// `D3AttachmentPool` firing them at the right one.
//
// It runs against the corpus rather than against a synthetic model, because
// there is no cheap synthetic here: the shortest path to a `ClipAttachments`
// call is an `.acr` that resolves an `.app` and an `.ans` that resolves an
// `.ani`, and hand-building that graph would be building a fake of exactly the
// thing under test. So the provider indexes the extracted tree by the SNO id in
// each file's header — 77,013 files, no collisions, which is the invariant
// D3SnoCache is built on — and the test drives the real adapter.
//
// Skips without `C:/Projects/WhiteoutLib/Corpus/D3` (override with
// WDX_TEST_D3_CORPUS). Skipped is not passed: the cases print what they found.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/d3/d3_effect_resolver.h"
#include "io/d3/d3_model_adapter.h"
#include "io/d3/d3_sno_cache.h"
#include "renderer/effects/d3_attachment_pool.h"
#include "renderer/model/model_instance.h"
#include "renderer/particle/particle_service.h"
#include "renderer/animation/anim_math.h"
#include "whiteout/flakes/pose_request.h"
#include "whiteout/flakes/content_provider.h"

#include <whiteout/sno/d3/native/types.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
namespace wio = ::whiteout::flakes::io;
namespace wd3 = ::whiteout::flakes::io::d3;
namespace wfx = ::whiteout::flakes::renderer::effects;
namespace wmodel = ::whiteout::flakes::renderer::model;
namespace wpart = ::whiteout::flakes::renderer::particle;
using namespace ::whiteout;

namespace {

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_D3_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/D3");
}

std::vector<u8> ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return {};
    f.seekg(0, std::ios::end);
    const auto n = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<u8> b(n);
    f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
    return b;
}

/// The extracted corpus, addressed the way the renderer addresses a real
/// install: by SNO id, with no idea which group the id belongs to. The index is
/// each file's own `dwSnoId`, at byte 16.
class CorpusProvider final : public wio::IContentProvider {
public:
    bool Index() {
        const fs::path root = CorpusRoot();
        std::error_code ec;
        if (!fs::is_directory(root, ec))
            return false;
        for (const char* g : {"Actor", "Appearances", "AnimSet", "Anim", "Particle",
                              "EffectGroup"}) {
            const fs::path d = root / g;
            if (!fs::is_directory(d, ec))
                continue;
            std::vector<fs::path> files;
            for (fs::directory_iterator it(d, ec), end; it != end; it.increment(ec)) {
                if (ec)
                    break;
                if (it->is_regular_file(ec))
                    files.push_back(it->path());
            }
            // Sorted so "the first actor with attachments" is a property of the
            // corpus and not of the filesystem's enumeration order.
            std::sort(files.begin(), files.end());
            auto& list = byGroup_[g];
            for (const auto& p : files) {
                std::ifstream f(p, std::ios::binary);
                char h[20];
                if (!f.read(h, 20))
                    continue;
                i32 sno = 0;
                std::memcpy(&sno, h + 16, 4);
                paths_.emplace(sno, p);
                list.push_back(sno);
            }
        }
        return !paths_.empty();
    }

    const std::vector<i32>& Group(const char* g) const {
        static const std::vector<i32> kEmpty;
        auto it = byGroup_.find(g);
        return it == byGroup_.end() ? kEmpty : it->second;
    }

    wio::RequestId Request(const wio::ContentRef& ref, wio::CompletionCallback cb) override {
        wio::RequestResult r;
        if (ref.IsFileId()) {
            auto it = paths_.find(static_cast<i32>(ref.fileId));
            if (it != paths_.end()) {
                r.data = ReadAll(it->second);
                r.ok = !r.data.empty();
            }
        }
        cb(std::move(r));
        return ++next_;
    }
    void Wait(wio::RequestId) override {}
    void Cancel(wio::RequestId) override {}
    void Pump() override {}

private:
    std::unordered_map<i32, fs::path> paths_;
    std::unordered_map<std::string, std::vector<i32>> byGroup_;
    wio::RequestId next_ = 0;
};

/// The first actor, in SNO order, whose animation set carries a keyframed
/// attachment the resolver can do something with. Returns its adapter and the
/// sequence that carries them.
struct Found {
    std::shared_ptr<wio::D3ModelAdapter> adapter;
    i32 sno = -1;
    i32 sequence = -1;
    std::size_t attachments = 0;
    i32 firstMs = -1; ///< When the earliest usable one fires.
};

/// @p minTimeMs demands an attachment that is not on the clip's first
/// millisecond, so the crossing arithmetic is actually asked a question.
Found FindActorWithAttachments(CorpusProvider& prov, wio::D3SnoCache& cache, bool wantActorPayload,
                               int maxActors, i32 minTimeMs = 0) {
    Found found;
    int tried = 0;
    for (i32 sno : prov.Group("Actor")) {
        if (tried >= maxActors)
            break;
        // Lazy for the SEARCH only: `ClipAttachments` resolves the one clip it
        // is asked about, so scanning hundreds of actors does not pay for the
        // whole tag map of each.
        auto a = wio::D3ModelAdapter::LoadActorBySno(sno, cache, /*lazyClips=*/true);
        if (!a)
            continue;
        ++tried;
        const auto seqs = a->GetSequences();
        for (i32 s = 0; s < static_cast<i32>(seqs.size()); ++s) {
            const auto atts = a->ClipAttachments(s);
            if (atts.empty())
                continue;
            std::size_t usable = 0;
            i32 firstUsableMs = -1;
            for (const auto& att : atts) {
                std::vector<wd3::ResolvedEffect> fx;
                wd3::ExpandD3TriggerEvent(*att.event, cache, {}, fx);
                for (const auto& f : fx) {
                    if (wantActorPayload && f.kind != wd3::ResolvedEffect::Kind::Actor)
                        continue;
                    ++usable;
                    if (firstUsableMs < 0)
                        firstUsableMs = att.timeMs;
                }
            }
            if (usable == 0 || firstUsableMs < minTimeMs)
                continue;
            // Re-opened eagerly, which is what the renderer does
            // (`D3LazyAnimations` defaults off). It matters here and not only
            // for speed: a lazily-loaded clip advertises a nominal 1,000 ms
            // window until it resolves, and an attachment past that would be
            // outside the crossing window and never fire. Everything it needs
            // is already in the cache, so this is a second walk and not a
            // second read.
            found.adapter = wio::D3ModelAdapter::LoadActorBySno(sno, cache, /*lazyClips=*/false);
            if (!found.adapter)
                continue;
            found.sno = sno;
            found.sequence = s;
            found.attachments = usable;
            found.firstMs = firstUsableMs;
            return found;
        }
    }
    return found;
}

} // namespace

TEST_CASE("d3 attachment: a clip's attachments resolve to times inside it",
          "[d3][attachment][corpus]") {
    CorpusProvider prov;
    if (!prov.Index()) {
        WARN("no D3 corpus; skipping");
        return;
    }
    wio::D3SnoCache cache(&prov);
    const Found f = FindActorWithAttachments(prov, cache, /*wantActorPayload=*/false, 60);
    if (!f.adapter) {
        WARN("no actor with keyframed attachments in the first 60; skipping");
        return;
    }

    const auto seqs = f.adapter->GetSequences();
    const auto atts = f.adapter->ClipAttachments(f.sequence);
    const i32 durMs = seqs[static_cast<std::size_t>(f.sequence)].endMs;
    std::printf("[d3 att] actor #%d seq %d '%s' %dms: %zu attachments, %zu usable\n", f.sno,
                f.sequence, seqs[static_cast<std::size_t>(f.sequence)].name.c_str(), durMs,
                atts.size(), f.attachments);

    REQUIRE(!atts.empty());
    // Ascending, because the pool walks a time window and the adapter is what
    // promises the order.
    for (std::size_t i = 1; i < atts.size(); ++i)
        CHECK(atts[i - 1].timeMs <= atts[i].timeMs);
    for (const auto& a : atts) {
        CHECK(a.event != nullptr);
        CHECK(a.timeMs >= 0);
    }
    // A frame past the clip's end is authoring slop and exists (256 of 52,138
    // corpus-wide), so this bounds the *conversion* rather than the data: a
    // scaling error would put attachments orders of magnitude out.
    CHECK(atts.front().timeMs <= durMs * 4 + 1000);
}

TEST_CASE("d3 attachment: the pool fires at the frame and reuses on the loop",
          "[d3][attachment][corpus]") {
    CorpusProvider prov;
    if (!prov.Index()) {
        WARN("no D3 corpus; skipping");
        return;
    }
    wio::D3SnoCache cache(&prov);
    const Found f =
        FindActorWithAttachments(prov, cache, /*wantActorPayload=*/false, 400, /*minTimeMs=*/50);
    if (!f.adapter) {
        WARN("no actor with a late keyframed attachment in the first 400; skipping");
        return;
    }

    const auto seqs = f.adapter->GetSequences();
    const i32 durMs = std::max(1, seqs[static_cast<std::size_t>(f.sequence)].endMs);

    wmodel::Actor actor;
    actor.handle = 7;
    wpart::ParticleService particles;
    wfx::D3AttachmentPool pool;
    pool.Bind(f.adapter, &cache, /*firstEmitterId=*/100);

    // Before anything has been ticked, nothing is registered: an emitter that
    // exists is an emitter that emits, which is the reason the pool builds them
    // lazily rather than at bind.
    CHECK(particles.EmitterCount() == 0);

    // Up to the millisecond before the first attachment, and no further.
    // Nothing has been crossed, so nothing exists yet — the half-open window is
    // what makes "fires at T" mean T and not T-1.
    for (i32 t = 0; t < f.firstMs; t += 16)
        pool.Tick(actor, f.sequence, t, 0, durMs, &particles);
    pool.Tick(actor, f.sequence, f.firstMs - 1, 0, durMs, &particles);
    CHECK(particles.EmitterCount() == 0);

    // Now the rest of the lap, in 16 ms steps.
    for (i32 t = f.firstMs; t <= durMs; t += 16)
        pool.Tick(actor, f.sequence, t, 0, durMs, &particles);
    const i32 afterFirstLap = particles.EmitterCount();
    std::printf("[d3 att] actor #%d seq %d: %d emitters after one %dms lap, first at %dms\n",
                f.sno, f.sequence, afterFirstLap, durMs, f.firstMs);
    CHECK(afterFirstLap > 0);

    // A second lap re-fires the same attachments. The engine spawns a fresh
    // system each time; this restarts the standing one, so the emitter count
    // must not move — a viewer that grew one emitter per lap would be the bug
    // this asserts against.
    for (i32 t = 0; t <= durMs; t += 16)
        pool.Tick(actor, f.sequence, t, 0, durMs, &particles);
    CHECK(particles.EmitterCount() == afterFirstLap);

    // A different sequence uses its own entries and its own ids -- and takes
    // the old sequence's emitters with it on the way in. An attachment belongs
    // to the animation that fired it; without that rule a viewer flipping
    // through a monster's clips accumulates every effect it has ever played.
    const i32 other = (f.sequence + 1) % static_cast<i32>(seqs.size());
    if (other != f.sequence) {
        pool.Tick(actor, other, 0, 0, durMs, &particles);
        const i32 afterSwitch = particles.EmitterCount();
        CHECK(afterSwitch <= afterFirstLap);
        for (i32 t = 0; t <= durMs; t += 16)
            pool.Tick(actor, other, t, 0, durMs, &particles);

        // Coming back rebuilds the first sequence's emitters from scratch --
        // released means unbuilt, not disabled, so the lazy path runs again.
        pool.Tick(actor, f.sequence, 0, 0, durMs, &particles);
        for (i32 t = 0; t <= durMs; t += 16)
            pool.Tick(actor, f.sequence, t, 0, durMs, &particles);
        CHECK(particles.EmitterCount() == afterFirstLap);
    }
}

TEST_CASE("d3 attachment: leaving a sequence releases the child actors it spawned",
          "[d3][attachment][corpus]") {
    CorpusProvider prov;
    if (!prov.Index()) {
        WARN("no D3 corpus; skipping");
        return;
    }
    wio::D3SnoCache cache(&prov);
    const Found f = FindActorWithAttachments(prov, cache, /*wantActorPayload=*/true, 900);
    if (!f.adapter) {
        WARN("no actor with an Actor-payload attachment in the first 900; skipping");
        return;
    }

    const auto seqs = f.adapter->GetSequences();
    const i32 durMs = std::max(1, seqs[static_cast<std::size_t>(f.sequence)].endMs);

    wmodel::Actor actor;
    actor.handle = 13;
    wpart::ParticleService particles;
    wfx::D3AttachmentPool pool;
    pool.Bind(f.adapter, &cache, 100);

    for (i32 t = 0; t <= durMs; t += 16)
        pool.Tick(actor, f.sequence, t, 0, durMs, &particles);
    const auto pending = pool.TakePending();
    REQUIRE(!pending.empty());

    // Answer every request, as the FrameTicker does.
    u32 handle = 5000;
    std::vector<u32> spawned;
    for (const auto& p : pending) {
        pool.NoteChildSpawned(p.sequence, p.entry, handle);
        spawned.push_back(handle++);
    }
    CHECK(pool.TakeExpired().empty());

    // Switch clips. Every actor the old sequence stood up comes back out, once,
    // for the ticker to destroy -- and the entries forget them, so re-entering
    // the sequence spawns fresh ones rather than re-birthing corpses.
    const i32 other = (f.sequence + 1) % static_cast<i32>(seqs.size());
    if (other == f.sequence) {
        WARN("single-sequence actor; skipping the switch half");
        return;
    }
    pool.Tick(actor, other, 0, 0, durMs, &particles);
    const auto expired = pool.TakeExpired();
    std::printf("[d3 att] actor #%d: %zu child actors released leaving seq %d\n", f.sno,
                expired.size(), f.sequence);
    CHECK(expired.size() == spawned.size());
    for (u32 h : spawned)
        CHECK(std::find(expired.begin(), expired.end(), h) != expired.end());
    CHECK(pool.TakeExpired().empty());
    for (const auto& p : pending)
        CHECK(pool.ChildHandle(p.sequence, p.entry) == 0);
}

TEST_CASE("d3 attachment: a group 1 payload is reported as a child model, not an emitter",
          "[d3][attachment][corpus]") {
    CorpusProvider prov;
    if (!prov.Index()) {
        WARN("no D3 corpus; skipping");
        return;
    }
    wio::D3SnoCache cache(&prov);
    // Rarer than the particle kind — 783 attachments over the whole corpus —
    // so this looks further before giving up.
    const Found f = FindActorWithAttachments(prov, cache, /*wantActorPayload=*/true, 900);
    if (!f.adapter) {
        WARN("no actor with an Actor-payload attachment in the first 900; skipping");
        return;
    }

    const auto seqs = f.adapter->GetSequences();
    const i32 durMs = std::max(1, seqs[static_cast<std::size_t>(f.sequence)].endMs);

    wmodel::Actor actor;
    actor.handle = 11;
    wpart::ParticleService particles;
    wfx::D3AttachmentPool pool;
    pool.Bind(f.adapter, &cache, 100);

    for (i32 t = 0; t <= durMs; t += 16)
        pool.Tick(actor, f.sequence, t, 0, durMs, &particles);

    const auto pending = pool.TakePending();
    std::printf("[d3 att] actor #%d seq %d '%s': %zu child models requested\n", f.sno, f.sequence,
                seqs[static_cast<std::size_t>(f.sequence)].name.c_str(), pending.size());
    REQUIRE(!pending.empty());
    for (const auto& p : pending) {
        CHECK(p.snoActor > 0);
        CHECK(p.existing == 0); // nothing has answered yet
        CHECK(p.sequence == f.sequence);
        CHECK(p.entry >= 0);
        // The `.acr` it names must be a real one, or the spawn would fail on
        // every lap. This is the join the census counts and the loader trusts.
        CHECK(cache.Actor(p.snoActor) != nullptr);
    }

    // Drained: the pool reports each request once, and the FrameTicker owns
    // what happens next.
    CHECK(pool.TakePending().empty());

    // Answering the request stops it being re-asked as a spawn; the ticker
    // re-births the standing child instead.
    pool.NoteChildSpawned(pending[0].sequence, pending[0].entry, 4242);
    CHECK(pool.ChildHandle(pending[0].sequence, pending[0].entry) == 4242);
    for (i32 t = 0; t <= durMs; t += 16)
        pool.Tick(actor, f.sequence, t, 0, durMs, &particles);
    const auto again = pool.TakePending();
    bool sawExisting = false;
    for (const auto& p : again)
        if (p.entry == pending[0].entry)
            sawExisting = (p.existing == 4242);
    CHECK(sawExisting);
}

// ---------------------------------------------------------------------------
// The hardpoint frame. Here rather than in `d3_effect_test` because proving it
// needs a real skeleton: the invariant is about how a hardpoint composes with
// the bone it names, and the synthetic case there can only show the arithmetic.
// ---------------------------------------------------------------------------

TEST_CASE("d3 attachment: a hardpoint's frame is its bone's, not its bone twice",
          "[d3][attachment][corpus]") {
    CorpusProvider prov;
    if (!prov.Index()) {
        WARN("no D3 corpus; skipping");
        return;
    }
    wio::D3SnoCache cache(&prov);

    auto tlen = [](const whiteout::Matrix44f& m) {
        return std::sqrt(m.data[3][0] * m.data[3][0] + m.data[3][1] * m.data[3][1] +
                         m.data[3][2] * m.data[3][2]);
    };
    auto sameT = [](const whiteout::Vector3f& a, const whiteout::Vector3f& b) {
        return std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f &&
               std::fabs(a.z - b.z) < 1e-4f;
    };

    int onBone = 0, ridesItsBone = 0, identity = 0, missed = 0;
    double sumAuthored = 0.0, sumResolved = 0.0;
    for (i32 sno : prov.Group("Appearances")) {
        auto app = cache.Appearance(sno);
        if (!app || app->arBones.empty())
            continue;
        for (const auto& h : app->arHardpoints) {
            const auto at = wd3::ResolveD3Attach(*app, h.szName);
            if (at.bone < 0 || (std::size_t)at.bone >= app->arBones.size())
                continue;
            ++onBone;
            const auto& t = h.tTransform.vTranslation;
            sumAuthored += std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z);
            const f32 resolved = tlen(at.offset);
            sumResolved += resolved;
            if (resolved < 1e-3f)
                ++identity;
            // A hardpoint authored ON its bone carries that bone's own
            // `tTransform0` verbatim, and `tTransform1` is its inverse — so
            // the two must cancel. This is the direction that fails loudly if
            // the composition is dropped or inverted.
            if (sameT(t, app->arBones[(std::size_t)at.bone].tTransform0.vTranslation)) {
                ++ridesItsBone;
                if (resolved >= 1e-3f)
                    ++missed;
            }
        }
    }
    REQUIRE(onBone > 20000);
    std::printf("[d3 hp]  %d hardpoints on a bone, %d resolve to identity; %d ride their own "
                "bone, %d of those did not cancel\n", onBone, identity, ridesItsBone, missed);
    std::printf("[d3 hp]  mean |translation|: %.3f authored -> %.3f resolved\n",
                sumAuthored / onBone, sumResolved / onBone);

    // Every hardpoint that rides its own bone cancels exactly. Composing the
    // hardpoint straight onto the bone leaves all 12,373 of them displaced by
    // the bone's own offset, which is the bug this gates.
    CHECK(missed == 0);
    CHECK(ridesItsBone * 2 > onBone);
    // And across the whole population the authored translation is mostly the
    // bone's own depth in the skeleton, not a displacement from it.
    CHECK(sumResolved * 3.0 < sumAuthored);
}

TEST_CASE("d3 attachment: a held weapon lands on the hand, in a real pose",
          "[d3][attachment][corpus]") {
    CorpusProvider prov;
    if (!prov.Index()) {
        WARN("no D3 corpus; skipping");
        return;
    }
    wio::D3SnoCache cache(&prov);
    // Tyrael by name: his `.acr` spawns El'Druin as a group 1 payload on
    // `HP_rightWeapon`, which is the whole chain — event, hardpoint, child
    // model — in one shipped file.
    const auto bytes = ReadAll(CorpusRoot() / "Actor" / "Tyrael.acr");
    if (bytes.size() < 20) {
        WARN("no Tyrael.acr; skipping");
        return;
    }
    i32 sno = 0;
    std::memcpy(&sno, bytes.data() + 16, 4);
    auto acr = cache.Actor(sno);
    auto adapter = wio::D3ModelAdapter::LoadActorBySno(sno, cache, /*lazyClips=*/false);
    REQUIRE(acr != nullptr);
    REQUIRE(adapter != nullptr);
    const auto& app = adapter->SourceAppearance();

    std::string_view look;
    if (adapter->LookIndex() < adapter->Looks().size())
        look = adapter->Looks()[adapter->LookIndex()];
    const auto fx = wd3::ResolveActorEffects(*acr, wd3::kD3MsgActorSpawned, cache, look);
    REQUIRE(fx.size() == 1);
    CHECK(fx[0].kind == wd3::ResolvedEffect::Kind::Actor);
    CHECK(fx[0].hardpoint == "HP_rightWeapon");

    whiteout::flakes::ClipRef cr;
    cr.sequence = 0;
    cr.timeMs = 0;
    cr.loop = true;
    cr.weight = 1.0f;
    const auto state = adapter->Evaluate(whiteout::flakes::PoseRequest::OneClip(cr));
    REQUIRE(state.boneWorldMatrices.size() == app.arBones.size());

    const auto at = wd3::ResolveD3Attach(app, fx[0].hardpoint);
    REQUIRE(at.bone >= 0);
    const whiteout::Matrix44f& bone = state.boneWorldMatrices[(std::size_t)at.bone];
    const whiteout::Matrix44f placed = at.offset * bone;

    f32 extent = 0.0f;
    for (const auto& m : state.boneWorldMatrices)
        extent = (std::max)(extent, std::sqrt(m.data[3][0] * m.data[3][0] +
                                              m.data[3][1] * m.data[3][1] +
                                              m.data[3][2] * m.data[3][2]));
    const f32 off = std::sqrt(
        (placed.data[3][0] - bone.data[3][0]) * (placed.data[3][0] - bone.data[3][0]) +
        (placed.data[3][1] - bone.data[3][1]) * (placed.data[3][1] - bone.data[3][1]) +
        (placed.data[3][2] - bone.data[3][2]) * (placed.data[3][2] - bone.data[3][2]));
    // What the composition costs if it is dropped: the hardpoint straight onto
    // the bone, which is what this used to do.
    const whiteout::Matrix44f raw = whiteout::flakes::renderer::animation::ComposePivotSRT(
        app.arHardpoints[(std::size_t)wd3::FindD3Hardpoint(app, fx[0].hardpoint)]
            .tTransform.vTranslation,
        whiteout::Quaternion{0.0f, 0.0f, 0.0f, -1.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f});
    const whiteout::Matrix44f wrong = raw * bone;
    const f32 strayed = std::sqrt(
        (wrong.data[3][0] - bone.data[3][0]) * (wrong.data[3][0] - bone.data[3][0]) +
        (wrong.data[3][1] - bone.data[3][1]) * (wrong.data[3][1] - bone.data[3][1]) +
        (wrong.data[3][2] - bone.data[3][2]) * (wrong.data[3][2] - bone.data[3][2]));
    std::printf("[d3 hp]  Tyrael '%s' -> bone %d, %.4f from it; uncomposed it strays %.2f, "
                "and the whole skeleton reaches %.2f\n",
                std::string(fx[0].hardpoint).c_str(), at.bone, off, strayed, extent);

    // `HP_rightWeapon` is authored on `right_weapon`, so the sword goes exactly
    // where that bone is. Composing it onto the bone instead throws it as far
    // again as the entire skeleton reaches from the origin, which is what put
    // El'Druin on the floor beside Tyrael rather than in his hand.
    CHECK(off < 1e-3f);
    CHECK(strayed > extent);
}
