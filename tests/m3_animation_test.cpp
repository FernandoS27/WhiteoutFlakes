// ============================================================================
// `.m3` track resolution and sampling.
//
// Two halves, like m2_animation_test: synthetic fixtures that pin the
// behaviours the StarCraft II runtime actually has, and a corpus sweep that
// checks shipped files agree with the assumptions the reader is built on.
//
// The synthetic half matters more than usual here. Several of these
// behaviours are ones a from-first-principles implementation gets wrong in a
// way that looks fine — a slerp instead of a componentwise lerp, wrapping on
// the sequence length instead of the track's, a normalised weighted mean
// instead of the smoothstep chain. Each has a case below naming the binary it
// was read out of.
// ============================================================================

#include "io/m3/m3_animation.h"
#include "m3_anim_builders.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/m3/parser.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace whiteout;
using namespace whiteout::flakes::io;
using Catch::Approx;

namespace {

std::vector<i32> Times(std::initializer_list<i32> t) {
    return std::vector<i32>(t);
}

} // namespace

// ---------------------------------------------------------------------------
// Key location
// ---------------------------------------------------------------------------

TEST_CASE("M3LocateKey brackets and clamps", "[m3anim]") {
    const auto times = Times({0, 100, 300});

    SECTION("empty track is not sampleable") {
        REQUIRE_FALSE(M3LocateKey({}, 50, false, true).valid);
    }
    SECTION("single key pins regardless of time") {
        const auto one = Times({250});
        const auto s = M3LocateKey(one, 9999, false, true);
        REQUIRE(s.valid);
        REQUIRE(s.i0 == 0);
        REQUIRE(s.i1 == 0);
    }
    SECTION("before the first key holds it") {
        const auto s = M3LocateKey(times, -50, false, true);
        REQUIRE(s.i0 == 0);
        REQUIRE(s.i1 == 0);
    }
    SECTION("after the last key holds it") {
        const auto s = M3LocateKey(times, 5000, false, true);
        REQUIRE(s.i0 == 2);
        REQUIRE(s.i1 == 2);
    }
    SECTION("interior brackets with a fraction") {
        const auto s = M3LocateKey(times, 200, false, true);
        REQUIRE(s.i0 == 1);
        REQUIRE(s.i1 == 2);
        REQUIRE(s.frac == Approx(0.5f));
    }
    SECTION("a step track holds the left key") {
        const auto s = M3LocateKey(times, 200, false, /*interpolate*/ false);
        REQUIRE(s.i0 == 1);
        REQUIRE(s.i1 == 1);
    }
    SECTION("exactly on a key resolves to that key") {
        // Asserted as a resolved value rather than as indices: landing on a
        // key is representable either as (0,1,frac=1) or (1,2,frac=0), and
        // both name keys[1]. Only the value is contractual.
        const auto s = M3LocateKey(times, 100, false, true);
        REQUIRE(s.valid);
        const std::vector<f32> keys = {10.0f, 20.0f, 30.0f};
        const f32 v = keys[s.i0] + (keys[s.i1] - keys[s.i0]) * s.frac;
        REQUIRE(v == Approx(20.0f));
    }
}

TEST_CASE("A looping track wraps on its own duration, not the sequence's", "[m3anim]") {
    // The behaviour ClipRef::elapsedMs exists to make expressible. This track
    // is 300 ms long; at 700 ms elapsed it must read 100 ms, whatever the
    // sequence around it is doing.
    const auto times = Times({0, 100, 300});
    const std::vector<f32> keys = {10.0f, 20.0f, 30.0f};
    const auto s = M3LocateKey(times, 700, /*loop*/ true, true);
    REQUIRE(s.valid);
    REQUIRE(keys[s.i0] + (keys[s.i1] - keys[s.i0]) * s.frac == Approx(20.0f));

    const auto s2 = M3LocateKey(times, 500, true, true);
    REQUIRE(s2.i0 == 1);
    REQUIRE(s2.i1 == 2);
    REQUIRE(s2.frac == Approx(0.5f)); // 500 % 300 = 200

    SECTION("not looping clamps instead") {
        const auto c = M3LocateKey(times, 700, false, true);
        REQUIRE(c.i0 == 2);
        REQUIRE(c.i1 == 2);
    }
    SECTION("negative time wraps forward") {
        const auto n = M3LocateKey(times, -100, true, true);
        REQUIRE(n.valid);
        REQUIRE(n.i0 == 1); // -100 % 300 -> 200
    }
}

// ---------------------------------------------------------------------------
// Interpolation policy
// ---------------------------------------------------------------------------

TEST_CASE("Track-level quaternion interpolation is a raw componentwise lerp", "[m3anim]") {
    // M3Anim_EvalTrackQuat does a plain SSE lerp: no normalisation, no
    // shortest-arc sign flip. Two keys 180 degrees apart make the difference
    // from a slerp unmissable.
    const Quaternion a{0.0f, 0.0f, 0.0f, 1.0f};
    const Quaternion b{0.0f, 0.0f, 1.0f, 0.0f};

    const Quaternion mid = M3LerpQuatRaw(a, b, 0.5f);
    REQUIRE(mid.x == Approx(0.0f));
    REQUIRE(mid.y == Approx(0.0f));
    REQUIRE(mid.z == Approx(0.5f));
    REQUIRE(mid.w == Approx(0.5f));

    // Unnormalised on purpose: |mid| is sqrt(0.5), not 1.
    const f32 len = std::sqrt(mid.x * mid.x + mid.y * mid.y + mid.z * mid.z + mid.w * mid.w);
    REQUIRE(len == Approx(0.70710678f).margin(1e-5));

    SECTION("and no hemisphere correction") {
        // A slerp would flip the sign of `c` to take the short way round; the
        // track lerp must not.
        const Quaternion c{0.0f, 0.0f, 0.0f, -1.0f};
        const Quaternion m = M3LerpQuatRaw(a, c, 0.5f);
        REQUIRE(m.w == Approx(0.0f));
        REQUIRE(m.x == Approx(0.0f));
    }
}

TEST_CASE("Cross-layer quaternion combination does slerp, with the sign fix", "[m3anim]") {
    const Quaternion a{0.0f, 0.0f, 0.0f, 1.0f};
    const Quaternion c{0.0f, 0.0f, 0.0f, -1.0f};
    // Same orientation the long way; the slerp must take the short way and
    // stay put rather than travelling through zero.
    const Quaternion m = M3SlerpQuat(a, c, 0.5f);
    const f32 len = std::sqrt(m.x * m.x + m.y * m.y + m.z * m.z + m.w * m.w);
    REQUIRE(len == Approx(1.0f).margin(1e-4));
}

// ---------------------------------------------------------------------------
// The blend chain
// ---------------------------------------------------------------------------

TEST_CASE("The combine factor is smoothstepped, not a normalised mean", "[m3anim]") {
    // acc = 0.6 already placed, adding w = 0.4:
    //   t = 0.4 / 1.0 = 0.4
    //   f = 0.4^2 * (3 - 0.8) = 0.16 * 2.2 = 0.352
    // A plain weighted mean would use 0.4. The gap is the whole point.
    REQUIRE(M3SmoothstepFactor(0.6f, 0.4f) == Approx(0.352f));

    SECTION("the first contribution takes everything") {
        REQUIRE(M3SmoothstepFactor(0.0f, 1.0f) == Approx(1.0f));
    }
    SECTION("an even split is not one half") {
        // t = 0.5 -> 0.25 * 2 = 0.5. This one happens to agree; the curve is
        // symmetric about the midpoint.
        REQUIRE(M3SmoothstepFactor(0.5f, 0.5f) == Approx(0.5f));
    }
    SECTION("a small contribution is pulled smaller still") {
        // t = 0.1 -> 0.01 * 2.8 = 0.028, well under the linear 0.1.
        REQUIRE(M3SmoothstepFactor(0.9f, 0.1f) == Approx(0.028f));
    }
    SECTION("zero total is a no-op guard") {
        REQUIRE(M3SmoothstepFactor(0.0f, 0.0f) == Approx(1.0f));
    }
}

// ---------------------------------------------------------------------------
// Table construction
// ---------------------------------------------------------------------------

TEST_CASE("M3AnimTables resolves the STC indirection", "[m3anim]") {
    const m3::Model model = m3fix::SplitBodyFixture();
    M3AnimTables t;
    t.Build(model);

    REQUIRE(t.StcCount() == 2);
    REQUIRE(t.RowCount() == 2); // two distinct animIds

    const i32 lowerRow = t.RowOf(m3fix::kLowerPosAnimId);
    const i32 upperRow = t.RowOf(m3fix::kUpperPosAnimId);
    REQUIRE(lowerRow >= 0);
    REQUIRE(upperRow >= 0);
    REQUIRE(lowerRow != upperRow);
    REQUIRE(t.RowOf(4242) == -1); // an animId nothing drives

    SECTION("each container resolves the same animId independently") {
        // Container 0 (lower) drives both properties; container 1 (upper)
        // drives only the upper one. That asymmetry is what the transparent
        // layer rule acts on.
        REQUIRE(t.At(lowerRow, 0).Valid());
        REQUIRE(t.At(upperRow, 0).Valid());
        REQUIRE_FALSE(t.At(lowerRow, 1).Valid());
        REQUIRE(t.At(upperRow, 1).Valid());
    }

    SECTION("out-of-range lookups are misses, not reads") {
        REQUIRE_FALSE(t.At(lowerRow, 99).Valid());
        REQUIRE_FALSE(t.At(-1, 0).Valid());
    }

    SECTION("layers come back priority-descending") {
        const auto layers = t.LayersFor(0);
        REQUIRE(layers.size() == 2);
        REQUIRE(layers[0].priority == 2);
        REQUIRE(layers[0].transparent); // the concurrent upper body
        REQUIRE(layers[1].priority == 1);
        REQUIRE_FALSE(layers[1].transparent);
    }

    SECTION("a sequence with no group has no layers") {
        REQUIRE(t.LayersFor(7).empty());
    }
}

TEST_CASE("M3AnimTables reports a track's own duration", "[m3anim]") {
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);
    m3fix::StcBuilder s("s", 0, false);
    s.Vec3(50, m3fix::Block<Vector3f>({0, 250, 800}, {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}));
    const u32 idx = mb.AddStc(s.Build());
    mb.Sequence("Stand", 0, 1000, {idx});
    const m3::Model model = mb.Build();

    M3AnimTables t;
    t.Build(model);
    const i32 row = t.RowOf(50);
    REQUIRE(row >= 0);
    // 800, not the sequence's 1000 — the distinction the wrap depends on.
    REQUIRE(t.DurationOf(model, 0, t.At(row, 0)) == 800);
}

TEST_CASE("A malformed animRef word resolves to no track rather than reading out of range",
          "[m3anim]") {
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);
    m3::SubTrackContainer stc;
    stc.name = "bad";
    stc.animIds = {11, 12, 13};
    stc.animRefs = {
        m3fix::AnimRefWord(m3fix::SdSlot::Vec3, 5), // block index past the end
        0x00FF0000u,                                // slot 255, not a slot at all
        m3fix::AnimRefWord(m3fix::SdSlot::Quat, 0), // slot exists, array empty
    };
    stc.sd3v.push_back(m3fix::Block<Vector3f>({0}, {{1, 1, 1}}));
    const u32 idx = mb.AddStc(std::move(stc));
    mb.Sequence("Stand", 0, 1000, {idx});
    const m3::Model model = mb.Build();

    M3AnimTables t;
    t.Build(model);
    REQUIRE_FALSE(t.At(t.RowOf(11), 0).Valid());
    REQUIRE_FALSE(t.At(t.RowOf(12), 0).Valid());
    REQUIRE_FALSE(t.At(t.RowOf(13), 0).Valid());
}

// ---------------------------------------------------------------------------
// Corpus sweep — the assumptions the reader is built on
// ---------------------------------------------------------------------------

namespace {

namespace fs = std::filesystem;

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_SC2_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus");
}

// The same four extracted sets m3_geometry_test walks.
constexpr const char* kCorpora[] = {"Sc2M3", "Sc2BetaM3", "StarM3", "HotSM3"};

std::size_t PerCorpusLimit() {
    if (const char* v = std::getenv("WDX_TEST_M3_LIMIT"); v && *v)
        return static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
    return 400;
}

std::vector<fs::path> FindModels(const fs::path& dir, std::size_t limit) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (!it->is_regular_file(ec))
            continue;
        if (it->path().extension() == ".m3")
            out.push_back(it->path());
        if (out.size() >= limit)
            break;
    }
    return out;
}

} // namespace

TEST_CASE("every corpus .m3 agrees with the animRef encoding", "[m3anim][corpus]") {
    const fs::path root = CorpusRoot();
    const std::size_t limit = PerCorpusLimit();

    std::vector<fs::path> files;
    for (const char* c : kCorpora) {
        const auto found = FindModels(root / c, limit);
        files.insert(files.end(), found.begin(), found.end());
    }
    if (files.empty()) {
        WARN("no .m3 corpus under " << root.string() << " — skipping");
        return;
    }

    std::size_t models = 0, withAnim = 0, badWord = 0, nonZeroStart = 0, groupMismatch = 0;
    std::size_t checkedRefs = 0;

    for (const auto& path : files) {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            continue;
        std::vector<u8> bytes((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
        if (bytes.empty())
            continue;

        m3::Model model;
        try {
            m3::Parser parser;
            model = parser.parse(bytes);
        } catch (...) {
            continue;
        }
        ++models;
        if (model.subTrackCollections.empty())
            continue;
        ++withAnim;

        // O8: the animRefs word really is (slot << 16) | blockIndex, with the
        // slot in range and the block index inside the array it names.
        for (const auto& stc : model.subTrackCollections) {
            const std::size_t n = std::min(stc.animIds.size(), stc.animRefs.size());
            for (std::size_t k = 0; k < n; ++k) {
                ++checkedRefs;
                const u32 w = stc.animRefs[k];
                const u32 slot = w >> 16;
                const u32 block = w & 0xFFFFu;
                if (slot > 12) {
                    ++badWord;
                    continue;
                }
                std::size_t arrSize = 0;
                switch (slot) {
                case 0: arrSize = stc.sdev.size(); break;
                case 1: arrSize = stc.sd2v.size(); break;
                case 2: arrSize = stc.sd3v.size(); break;
                case 3: arrSize = stc.sd4q.size(); break;
                case 4: arrSize = stc.sdcc.size(); break;
                case 5: arrSize = stc.sdr3.size(); break;
                case 6: arrSize = stc.sdu8.size(); break;
                case 7: arrSize = stc.sds6.size(); break;
                case 8: arrSize = stc.sdu6.size(); break;
                case 9: arrSize = stc.sds3.size(); break;
                case 10: arrSize = stc.sdu3.size(); break;
                case 11: arrSize = stc.sdfg.size(); break;
                default: arrSize = stc.sdmb.size(); break;
                }
                if (block >= arrSize)
                    ++badWord;
            }
        }

        // O6: is SEQS.startFrame ever non-zero? The sample time is built as
        // startFrame + elapsed, so the answer decides whether that term ever
        // does anything.
        for (const auto& s : model.sequences)
            if (s.startFrame != 0)
                ++nonZeroStart;

        // The group array is assumed parallel to the sequence array.
        if (!model.animationGroups.empty() &&
            model.animationGroups.size() != model.sequences.size())
            ++groupMismatch;

        // The tables must build without tripping over any of it.
        M3AnimTables t;
        t.Build(model);
    }

    WARN("m3 corpus: " << models << " models, " << withAnim << " animated, " << checkedRefs
                       << " animRefs checked, " << nonZeroStart << " sequences with startFrame != 0, "
                       << groupMismatch << " group/sequence count mismatches");

    REQUIRE(models > 0);
    // The encoding is load-bearing: the reader indexes typed arrays with it.
    REQUIRE(badWord == 0);
}
