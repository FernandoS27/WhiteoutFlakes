// ============================================================================
// M2 animation — the sampler's rules, and the assumptions the adapter rests on.
//
// Two halves, as the other M2 tests split:
//
//   * Pure tables and hand-built tracks. No device, no corpus, always runs.
//     This is where a transcription slip in the compressed-quaternion decode or
//     the per-sequence sub-array fallback gets caught.
//   * Corpus sweep over `C:/Projects/WhiteoutLib/Corpus/WoW` (override with
//     WDX_TEST_WOW_CORPUS). Skips when the corpus is absent — skipped is not
//     passed.
//
// The corpus half exists for one claim in particular. `GetSkinWeights` reads
// bone indices off the `.m2` vertex record, because that is what
// `CM2Model::TransformVerticesNoUVSelect_cpp` indexes the bone-matrix array
// with. The `.skin` carries a second, section-local set that the client's
// shader path substitutes (`CM2Shared::SetVerticesShader`) and resolves through
// `boneCombos[section.boneComboIndex + local]`. The two must name the same
// bone; if they ever do not, this is the test that says so.
// ============================================================================

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "io/file_content_provider.h"
#include "io/m2/m2_animation.h"
#include "io/m2/m2_model_adapter.h"
#include "whiteout/flakes/content_ref.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
using whiteout::Vector3f;
using whiteout::flakes::ContentRef;
namespace io = whiteout::flakes::io;
namespace wm2 = whiteout::m2;
namespace fs = std::filesystem;

namespace {

// One sequence's worth of keys, in the array-of-arrays shape the format uses.
template <class T>
wm2::AnimationTrack<T> MakeTrack(std::vector<std::vector<whiteout::u32>> ts,
                                 std::vector<std::vector<T>> vals,
                                 wm2::InterpolationType interp = wm2::InterpolationType::Linear,
                                 whiteout::u16 globalSeq = 0xFFFF) {
    wm2::AnimationTrack<T> t;
    t.interpolationType = interp;
    t.globalSequenceId = globalSeq;
    t.timestamps = std::move(ts);
    t.values = std::move(vals);
    return t;
}

io::M2AnimTime At(whiteout::i32 seq, whiteout::i32 ms) {
    io::M2AnimTime a;
    a.sequence = seq;
    a.timeMs = ms;
    return a;
}

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

} // namespace

TEST_CASE("M2 compressed quaternion decodes across the full range", "[m2][anim]") {
    // `raw * (2/65535) - 1`, straight off M2AnimateTrack<M2CompQuat, C4Quaternion>.
    // Note what this is NOT: the wiki's `(v < 0 ? v + 32768 : v - 32767)/32767`
    // reads the word as signed and would map 0 to -1.0 by a different route and
    // 32767 to 0.0 rather than to -1.5e-5.
    CHECK_THAT(io::M2DecodeQuatComponent(0), WithinAbs(-1.0f, 1e-6f));
    CHECK_THAT(io::M2DecodeQuatComponent(65535), WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(io::M2DecodeQuatComponent(32768), WithinAbs(0.0f, 1e-4f));

    wm2::CompatQuaternion q{0, 32768, 65535, 32768};
    const auto d = io::M2DecodeQuat(q);
    CHECK_THAT(d.x, WithinAbs(-1.0f, 1e-4f));
    CHECK_THAT(d.y, WithinAbs(0.0f, 1e-4f));
    CHECK_THAT(d.z, WithinAbs(1.0f, 1e-4f));
    CHECK_THAT(d.w, WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("M2 fixed16 decodes to [0,1]", "[m2][anim]") {
    CHECK_THAT(io::M2DecodeFixed16(0), WithinAbs(0.0f, 1e-6f));
    CHECK_THAT(io::M2DecodeFixed16(32767), WithinAbs(1.0f, 1e-6f));
    CHECK_THAT(io::M2DecodeFixed16(16384), WithinAbs(0.5f, 1e-4f));
}

TEST_CASE("M2 track sampling interpolates, steps and holds", "[m2][anim]") {
    const auto track = MakeTrack<Vector3f>({{0, 100, 200}},
                                           {{{0, 0, 0}, {10, 0, 0}, {10, 20, 0}}});

    SECTION("exact keys") {
        CHECK_THAT(io::SampleM2Vec3(track, At(0, 0), {}).x, WithinAbs(0.0f, 1e-5f));
        CHECK_THAT(io::SampleM2Vec3(track, At(0, 100), {}).x, WithinAbs(10.0f, 1e-5f));
    }
    SECTION("between keys") {
        const auto v = io::SampleM2Vec3(track, At(0, 150), {});
        CHECK_THAT(v.x, WithinAbs(10.0f, 1e-5f));
        CHECK_THAT(v.y, WithinAbs(10.0f, 1e-5f));
    }
    SECTION("past the last key holds it") {
        CHECK_THAT(io::SampleM2Vec3(track, At(0, 9999), {}).y, WithinAbs(20.0f, 1e-5f));
    }
    SECTION("before the first key holds it") {
        // Clamped, unlike the client's linear-walk branch, which extrapolates
        // backwards. Every shipped track starts at 0, so the two never differ
        // on real data.
        CHECK_THAT(io::SampleM2Vec3(track, At(0, -500), {}).x, WithinAbs(0.0f, 1e-5f));
    }
    SECTION("interpolationType None steps") {
        const auto stepped = MakeTrack<Vector3f>({{0, 100}}, {{{0, 0, 0}, {10, 0, 0}}},
                                                 wm2::InterpolationType::None);
        CHECK_THAT(io::SampleM2Vec3(stepped, At(0, 99), {}).x, WithinAbs(0.0f, 1e-5f));
    }
}

TEST_CASE("M2 track picks its sequence's keys, falling back to sub-array 0", "[m2][anim]") {
    const auto track = MakeTrack<Vector3f>({{0}, {0}}, {{{1, 0, 0}}, {{2, 0, 0}}});
    CHECK_THAT(io::SampleM2Vec3(track, At(0, 0), {}).x, WithinAbs(1.0f, 1e-5f));
    CHECK_THAT(io::SampleM2Vec3(track, At(1, 0), {}).x, WithinAbs(2.0f, 1e-5f));
    // Past the end of the array — an alias, or a sequence this track was never
    // authored for. The client clamps the index to 0 rather than dropping the
    // track, which is what makes an aliased sequence animate at all.
    CHECK_THAT(io::SampleM2Vec3(track, At(7, 0), {}).x, WithinAbs(1.0f, 1e-5f));
}

TEST_CASE("M2 track with no keys for a sequence answers the default", "[m2][anim]") {
    // An empty sub-array is the branch M2AnimateTrack takes to the animref
    // default — not to the previous key, and not to zero.
    const auto track = MakeTrack<Vector3f>({{}, {0}}, {{}, {{5, 0, 0}}});
    const Vector3f def{9, 9, 9};
    CHECK_THAT(io::SampleM2Vec3(track, At(0, 0), def).x, WithinAbs(9.0f, 1e-5f));
    CHECK_THAT(io::SampleM2Vec3(track, At(1, 0), def).x, WithinAbs(5.0f, 1e-5f));
}

TEST_CASE("M2 global-sequence tracks ignore the clip and wrap on their period",
          "[m2][anim]") {
    // globalSequenceId 0, period 1000ms. The clip time must not reach it.
    const auto track =
        MakeTrack<Vector3f>({{0, 500}}, {{{0, 0, 0}, {10, 0, 0}}},
                            wm2::InterpolationType::Linear, /*globalSeq=*/0);
    const std::vector<whiteout::u32> loops{1000};

    io::M2AnimTime at;
    at.sequence = 3; // deliberately not 0: a global track reads sub-array 0
    at.timeMs = 250; // and must ignore this
    at.globalLoops = std::span<const whiteout::u32>(loops);

    at.globalTimeMs = 250;
    CHECK_THAT(io::SampleM2Vec3(track, at, {}).x, WithinAbs(5.0f, 1e-5f));
    // 1250 wraps to 250, giving the same sample — the wrap is the point.
    at.globalTimeMs = 1250;
    CHECK_THAT(io::SampleM2Vec3(track, at, {}).x, WithinAbs(5.0f, 1e-5f));
}

TEST_CASE("M2 animation names match the client's table", "[m2][anim]") {
    CHECK(io::M2AnimationName(0) == "Stand");
    CHECK(io::M2AnimationName(1) == "Death");
    CHECK(io::M2AnimationName(5) == "Run");
    CHECK(io::M2AnimationName(69) == "EmoteDance");
    CHECK(io::M2AnimationName(0xFFFF).empty());
}

TEST_CASE("every corpus .m2 animates coherently", "[m2][anim]") {
    const auto models = FindModels();
    if (models.empty()) {
        SKIP("no .m2 files under " + CorpusRoot().string() +
             " (set WDX_TEST_WOW_CORPUS to point elsewhere)");
    }

    io::FileContentProvider provider;
    std::size_t checked = 0;

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

        const auto& model = adapter->SourceModel();

        // The skeleton, and the invariant one forward pass rests on. The client
        // asserts `shared->parentIndex < boneIndex` for the same reason.
        const auto skel = adapter->GetSkeleton();
        CHECK(skel.nodeCount >= 1);
        CHECK(skel.inverseBindMatrices.size() == static_cast<std::size_t>(skel.nodeCount));
        for (std::size_t i = 0; i < model.bones.size(); ++i) {
            const auto parent = skel.nodeParents[i];
            CHECK(parent < static_cast<whiteout::i32>(i));
        }

        // Sequences: never empty, and every one has a usable window.
        const auto seqs = adapter->GetSequences();
        REQUIRE_FALSE(seqs.empty());
        for (const auto& s : seqs) {
            CHECK_FALSE(s.name.empty());
            CHECK(s.endMs >= s.startMs);
        }

        // Skin weights line up with the geometry one geoset at a time — the
        // loader only builds a bone stream when the counts match exactly, so a
        // mismatch silently disables skinning rather than failing loudly.
        const auto meshes = const_cast<io::M2ModelAdapter&>(*adapter).GetMeshes();
        const auto weights = const_cast<io::M2ModelAdapter&>(*adapter).GetSkinWeights();
        REQUIRE(weights.size() == meshes.size());
        for (std::size_t g = 0; g < meshes.size(); ++g) {
            INFO("geoset " << meshes[g].geosetId);
            REQUIRE(weights[g].geosetId == meshes[g].geosetId);
            REQUIRE(weights[g].influences.size() == meshes[g].positions.size());
            REQUIRE_FALSE(weights[g].subsetNodeIndices.empty());
            // Every local slot resolves to a live bone, and every subset fits
            // the 256-slot palette the shader declares.
            CHECK(weights[g].subsetNodeIndices.size() <= 256);
            for (auto node : weights[g].subsetNodeIndices)
                REQUIRE(node < skel.nodeCount);
            for (const auto& inf : weights[g].influences)
                for (int k = 0; k < 4; ++k)
                    REQUIRE(inf.boneIdx[k] <
                            static_cast<whiteout::i32>(weights[g].subsetNodeIndices.size()));
        }

        // The two bone-index spaces agree. See the file header.
        const auto& skin = model.skinProfiles[adapter->ProfileIndex()];
        if (!skin.bones.empty()) {
            for (const auto& sec : skin.submeshes) {
                const std::size_t vEnd =
                    static_cast<std::size_t>(sec.vertexStart) + sec.vertexCount;
                if (vEnd > skin.vertices.size() || vEnd > skin.bones.size())
                    continue;
                for (std::size_t v = sec.vertexStart; v < vEnd; ++v) {
                    const std::size_t gv = skin.vertices[v];
                    if (gv >= model.vertices.size())
                        continue;
                    const auto& mv = model.vertices[gv];
                    for (int k = 0; k < 4; ++k) {
                        if (mv.boneWeights[k] == 0)
                            continue;
                        const std::size_t combo =
                            static_cast<std::size_t>(sec.boneComboIndex) + skin.bones[v][k];
                        if (combo >= model.boneCombos.size())
                            continue;
                        CHECK(model.boneCombos[combo] == mv.boneIndices[k]);
                    }
                }
            }
        }

        // Evaluate every sequence at three points. What this catches is an
        // out-of-range key index or a NaN leaking out of a degenerate track —
        // both of which produce a model that draws as a single point.
        for (whiteout::i32 s = 0; s < static_cast<whiteout::i32>(seqs.size()); ++s) {
            const whiteout::i32 dur = seqs[s].endMs - seqs[s].startMs;
            for (whiteout::i32 t : {0, dur / 2, dur}) {
                whiteout::flakes::ClipRef clip;
                clip.sequence = s;
                clip.timeMs = t;
                auto req = whiteout::flakes::PoseRequest::OneClip(clip);
                req.globalTimeMs = t;
                const auto fs = adapter->Evaluate(req);
                REQUIRE(fs.boneWorldMatrices.size() ==
                        static_cast<std::size_t>(skel.nodeCount));
                std::size_t badBone = fs.boneWorldMatrices.size();
                for (std::size_t b = 0; b < fs.boneWorldMatrices.size() &&
                                        badBone == fs.boneWorldMatrices.size();
                     ++b) {
                    const auto& m = fs.boneWorldMatrices[b];
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 4; ++c)
                            if (!std::isfinite(m.data[r][c]))
                                badBone = b;
                }
                INFO("sequence " << s << " (" << seqs[s].name << ") at " << t << "ms, bone "
                                 << badBone);
                REQUIRE(badBone == fs.boneWorldMatrices.size());
                REQUIRE(fs.texAnimMatrices.size() == model.textureTransforms.size());
                REQUIRE(fs.surfaceStates.size() == skin.batches.size());
                for (const auto& ss : fs.surfaceStates) {
                    CHECK(ss.alpha >= 0.0f);
                    CHECK(ss.alpha <= 1.0f);
                }
            }
        }
        ++checked;
    }
    CHECK(checked == models.size());
}


