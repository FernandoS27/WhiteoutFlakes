// ============================================================================
// Lazy `.anim` loading — does deferring the read change anything?
//
// The claim under test is narrow and total: a model parsed with
// Parser::setLazyAnimations, then told to load each sequence, holds exactly the
// keys an eager parse holds, and animates to exactly the same frame. If that
// ever stops being true the lazy path is not an option, it is a second
// behaviour.
//
// Everything here needs the corpus (`C:/Projects/WhiteoutLib/Corpus/WoW`,
// override with WDX_TEST_WOW_CORPUS) — the whole point is `.anim` siblings,
// which no hand-built fixture has. Skips when it is absent; skipped is not
// passed.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/file_content_provider.h"
#include "io/m2/m2_model_adapter.h"
#include "whiteout/flakes/content_ref.h"

#include <whiteout/models/m2/m2.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using whiteout::flakes::ContentRef;
namespace io = whiteout::flakes::io;
namespace wm2 = whiteout::m2;
namespace fs = std::filesystem;

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

/// A provider that records what was asked for, and forwards everything.
///
/// Request is the one funnel — ReadFile is a non-virtual convenience over
/// Request+Wait — so counting here counts every read the parser makes.
class CountingProvider final : public io::IContentProvider {
public:
    explicit CountingProvider(io::IContentProvider& inner) : inner_(inner) {}

    io::RequestId Request(const ContentRef& ref, io::CompletionCallback cb) override {
        std::string name = ref.IsFileId() ? std::string() : ref.path;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".anim") == 0)
            ++animReads;
        return inner_.Request(ref, std::move(cb));
    }
    void Wait(io::RequestId id) override {
        inner_.Wait(id);
    }
    void Cancel(io::RequestId id) override {
        inner_.Cancel(id);
    }
    void Pump() override {
        inner_.Pump();
    }

    std::size_t animReads = 0;

private:
    io::IContentProvider& inner_;
};

std::shared_ptr<io::M2ModelAdapter> LoadModel(io::IContentProvider& provider,
                                              const fs::path& path, bool lazy) {
    auto bytes = provider.ReadFile(path.string());
    if (!bytes)
        return nullptr;
    return io::M2ModelAdapter::Load(ContentRef::FromPath(path.string()),
                                    std::span<const whiteout::u8>(bytes->data(), bytes->size()),
                                    &provider, lazy);
}

/// Sub-arrays that hold at least one key, over one track.
template <class Track>
std::size_t KeyedSequences(const Track& track) {
    std::size_t n = 0;
    for (const auto& sub : track.values)
        n += sub.empty() ? 0u : 1u;
    return n;
}

template <class T>
void RequireSameKeys(const std::vector<std::vector<T>>& lazy,
                     const std::vector<std::vector<T>>& eager) {
    REQUIRE(lazy.size() == eager.size());
    for (std::size_t s = 0; s < eager.size(); ++s) {
        INFO("sequence " << s);
        REQUIRE(lazy[s].size() == eager[s].size());
        REQUIRE(std::memcmp(lazy[s].data(), eager[s].data(), lazy[s].size() * sizeof(T)) == 0);
    }
}

template <class Track>
void RequireSameTrack(const Track& lazy, const Track& eager) {
    REQUIRE(lazy.interpolationType == eager.interpolationType);
    REQUIRE(lazy.globalSequenceId == eager.globalSequenceId);
    RequireSameKeys(lazy.timestamps, eager.timestamps);
    RequireSameKeys(lazy.values, eager.values);
}

void RequireSameTracks(const wm2::Model& lazy, const wm2::Model& eager) {
    REQUIRE(lazy.bones.size() == eager.bones.size());
    for (std::size_t b = 0; b < eager.bones.size(); ++b) {
        INFO("bone " << b);
        RequireSameTrack(lazy.bones[b].translation, eager.bones[b].translation);
        RequireSameTrack(lazy.bones[b].rotation, eager.bones[b].rotation);
        RequireSameTrack(lazy.bones[b].scale, eager.bones[b].scale);
    }
    REQUIRE(lazy.textureTransforms.size() == eager.textureTransforms.size());
    for (std::size_t t = 0; t < eager.textureTransforms.size(); ++t) {
        INFO("texture transform " << t);
        RequireSameTrack(lazy.textureTransforms[t].translation,
                         eager.textureTransforms[t].translation);
        RequireSameTrack(lazy.textureTransforms[t].rotation, eager.textureTransforms[t].rotation);
        RequireSameTrack(lazy.textureTransforms[t].scaling, eager.textureTransforms[t].scaling);
    }
    REQUIRE(lazy.textureWeights.size() == eager.textureWeights.size());
    for (std::size_t w = 0; w < eager.textureWeights.size(); ++w) {
        INFO("texture weight " << w);
        RequireSameTrack(lazy.textureWeights[w].weight, eager.textureWeights[w].weight);
    }
    REQUIRE(lazy.colors.size() == eager.colors.size());
    for (std::size_t c = 0; c < eager.colors.size(); ++c) {
        INFO("color " << c);
        RequireSameTrack(lazy.colors[c].color, eager.colors[c].color);
        RequireSameTrack(lazy.colors[c].alpha, eager.colors[c].alpha);
    }
    REQUIRE(lazy.attachments.size() == eager.attachments.size());
    for (std::size_t a = 0; a < eager.attachments.size(); ++a) {
        INFO("attachment " << a);
        RequireSameTrack(lazy.attachments[a].animate, eager.attachments[a].animate);
    }
    REQUIRE(lazy.events.size() == eager.events.size());
    for (std::size_t e = 0; e < eager.events.size(); ++e) {
        INFO("event " << e);
        RequireSameKeys(lazy.events[e].enabled.timestamps, eager.events[e].enabled.timestamps);
    }
}

/// The renderer-visible half: the same pose, sampled from both models.
void RequireSamePose(const io::M2ModelAdapter& lazy, const io::M2ModelAdapter& eager,
                     whiteout::i32 sequence, whiteout::i32 timeMs) {
    whiteout::flakes::ClipRef clip;
    clip.sequence = sequence;
    clip.timeMs = timeMs;
    auto req = whiteout::flakes::PoseRequest::OneClip(clip);
    req.globalTimeMs = timeMs;

    const auto a = lazy.Evaluate(req);
    const auto b = eager.Evaluate(req);

    REQUIRE(a.boneWorldMatrices.size() == b.boneWorldMatrices.size());
    for (std::size_t i = 0; i < b.boneWorldMatrices.size(); ++i) {
        INFO("bone " << i);
        REQUIRE(std::memcmp(&a.boneWorldMatrices[i], &b.boneWorldMatrices[i],
                            sizeof(whiteout::Matrix44f)) == 0);
    }
    REQUIRE(a.texAnimMatrices.size() == b.texAnimMatrices.size());
    for (std::size_t i = 0; i < b.texAnimMatrices.size(); ++i) {
        INFO("tex anim " << i);
        REQUIRE(std::memcmp(&a.texAnimMatrices[i], &b.texAnimMatrices[i],
                            sizeof(b.texAnimMatrices[i])) == 0);
    }
    REQUIRE(a.surfaceStates.size() == b.surfaceStates.size());
    for (std::size_t i = 0; i < b.surfaceStates.size(); ++i) {
        INFO("surface " << i);
        REQUIRE(std::memcmp(&a.surfaceStates[i], &b.surfaceStates[i],
                            sizeof(b.surfaceStates[i])) == 0);
    }
}

} // namespace

TEST_CASE("a lazy parse reads no .anim sibling", "[m2][anim][lazy]") {
    const auto models = FindModels();
    if (models.empty()) {
        SKIP("no .m2 files under " + CorpusRoot().string() +
             " (set WDX_TEST_WOW_CORPUS to point elsewhere)");
    }

    io::FileContentProvider backing;
    std::size_t streamedModels = 0;

    for (const auto& path : models) {
        backing.SetBasePath(path.parent_path());
        INFO("model " << path.string());

        CountingProvider eagerCounts(backing);
        auto eager = LoadModel(eagerCounts, path, false);
        REQUIRE(eager);

        CountingProvider lazyCounts(backing);
        auto lazy = LoadModel(lazyCounts, path, true);
        REQUIRE(lazy);

        // The parse itself, whatever else it did, did not touch a `.anim`.
        CHECK(lazyCounts.animReads == 0);
        if (eagerCounts.animReads == 0)
            continue; // a model with no external sequences proves nothing here
        ++streamedModels;

        // And playing one sequence reads at most one file — the one that
        // sequence's keys are in. At most, because an alias shares a file and a
        // sequence may have none.
        auto& model = const_cast<wm2::Model&>(lazy->SourceModel());
        std::size_t pending = 0;
        for (whiteout::u32 s = 0; s < model.sequences.size(); ++s)
            pending += wm2::sequenceKeysPending(model, s) ? 1u : 0u;
        REQUIRE(pending > 0);

        // ...and playing one sequence reads exactly one file: the one that
        // sequence's keys are in, with no existence probe on the side.
        bool loadedOne = false;
        for (whiteout::u32 s = 0; s < model.sequences.size() && !loadedOne; ++s) {
            if (!wm2::sequenceKeysPending(model, s))
                continue;
            const std::size_t reads = lazyCounts.animReads;
            const bool ok = wm2::loadSequence(model, s);
            CHECK(lazyCounts.animReads == reads + 1);
            CHECK_FALSE(wm2::sequenceKeysPending(model, s));
            loadedOne = ok;
        }
        CHECK(loadedOne);

        // Loading the rest never costs more than one read each — a sequence
        // whose file is already in hand or absent costs none.
        for (whiteout::u32 s = 0; s < model.sequences.size(); ++s)
            wm2::loadSequence(model, s);
        CHECK(lazyCounts.animReads <= pending);
    }

    if (streamedModels == 0) {
        SKIP("no corpus model keeps sequences in `.anim` siblings; nothing to defer");
    }
}

TEST_CASE("a loaded lazy model holds exactly what an eager parse holds", "[m2][anim][lazy]") {
    const auto models = FindModels();
    if (models.empty()) {
        SKIP("no .m2 files under " + CorpusRoot().string() +
             " (set WDX_TEST_WOW_CORPUS to point elsewhere)");
    }

    io::FileContentProvider provider;
    std::size_t deferredTracks = 0;

    for (const auto& path : models) {
        provider.SetBasePath(path.parent_path());
        INFO("model " << path.string());

        auto eager = LoadModel(provider, path, false);
        auto lazy = LoadModel(provider, path, true);
        REQUIRE(eager);
        REQUIRE(lazy);
        REQUIRE(wm2::hasLazyAnimations(lazy->SourceModel()));
        REQUIRE_FALSE(wm2::hasLazyAnimations(eager->SourceModel()));

        auto& lazyModel = const_cast<wm2::Model&>(lazy->SourceModel());
        const auto& eagerModel = eager->SourceModel();
        REQUIRE(lazyModel.sequences.size() == eagerModel.sequences.size());

        // Before loading, the deferred keys really are absent — otherwise the
        // comparison below would pass on a model that never deferred anything.
        for (std::size_t b = 0; b < eagerModel.bones.size(); ++b) {
            const std::size_t eagerKeyed = KeyedSequences(eagerModel.bones[b].rotation);
            const std::size_t lazyKeyed = KeyedSequences(lazyModel.bones[b].rotation);
            REQUIRE(lazyKeyed <= eagerKeyed);
            deferredTracks += eagerKeyed - lazyKeyed;
        }

        for (whiteout::u32 s = 0; s < lazyModel.sequences.size(); ++s)
            wm2::loadSequence(lazyModel, s);

        RequireSameTracks(lazyModel, eagerModel);

        // And the same thing said in the renderer's terms: identical poses.
        for (whiteout::i32 s = 0; s < static_cast<whiteout::i32>(eagerModel.sequences.size());
             ++s) {
            const auto duration = static_cast<whiteout::i32>(eagerModel.sequences[s].duration);
            INFO("sequence " << s);
            RequireSamePose(*lazy, *eager, s, 0);
            RequireSamePose(*lazy, *eager, s, duration / 3);
            RequireSamePose(*lazy, *eager, s, duration);
        }
        RequireSamePose(*lazy, *eager, -1, 0); // bind pose
    }

    if (deferredTracks == 0) {
        SKIP("no corpus model defers bone keys; the comparison proves nothing");
    }
}

TEST_CASE("an unloaded sequence reloads to the same keys", "[m2][anim][lazy]") {
    const auto models = FindModels();
    if (models.empty()) {
        SKIP("no .m2 files under " + CorpusRoot().string() +
             " (set WDX_TEST_WOW_CORPUS to point elsewhere)");
    }

    io::FileContentProvider provider;
    std::size_t roundTripped = 0;

    for (const auto& path : models) {
        provider.SetBasePath(path.parent_path());
        INFO("model " << path.string());

        auto eager = LoadModel(provider, path, false);
        auto lazy = LoadModel(provider, path, true);
        REQUIRE(eager);
        REQUIRE(lazy);

        auto& lazyModel = const_cast<wm2::Model&>(lazy->SourceModel());
        for (whiteout::u32 s = 0; s < lazyModel.sequences.size(); ++s)
            wm2::loadSequence(lazyModel, s);

        // Unload puts back exactly the state the parse left, and only for the
        // sequences that came from a file — the ones the `.m2` carried cannot
        // be dropped, because nothing could read them again.
        std::vector<whiteout::u8> wasPending;
        wm2::unloadAllSequences(lazyModel);
        for (whiteout::u32 s = 0; s < lazyModel.sequences.size(); ++s) {
            if (wm2::sequenceKeysPending(lazyModel, s))
                ++roundTripped;
            wm2::loadSequence(lazyModel, s);
        }

        RequireSameTracks(lazyModel, eager->SourceModel());
    }

    if (roundTripped == 0) {
        SKIP("no corpus model has an unloadable sequence");
    }
}
