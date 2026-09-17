#include "features/sc2_animation_files.h"

#include "documents/document_manager.h"
#include "documents/playback_controller.h"
#include "io/m3/m3_model_adapter.h"
#include "renderer/model/model_instance.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <fstream>
#include <iterator>
#include <system_error>

namespace whiteout::flakes {

namespace {

namespace model = renderer::model;

// The focus actor's `.m3` source, or null when it is any other format. The
// adapter outlives the call: it is owned by the actor's AnimationDriver.
io::M3ModelAdapter* M3AdapterOf(model::Actor* hero) {
    if (!hero || !hero->animation.HasSource())
        return nullptr;
    return dynamic_cast<io::M3ModelAdapter*>(hero->animation.Source().get());
}

class Sc2AnimationFilesImpl final : public Sc2AnimationFiles {
public:
    Sc2AnimationFilesImpl(renderer::RenderService& service, DocumentManager& documents,
                          PlaybackController& playback)
        : service_(service), documents_(documents), playback_(playback) {}

    bool CanAttach() const override {
        return M3AdapterOf(playback_.FocusActor()) != nullptr;
    }

    std::vector<Attached> AttachedFiles() const override {
        std::vector<Attached> out;
        if (io::M3ModelAdapter* m3 = M3AdapterOf(playback_.FocusActor()))
            for (const auto& a : m3->AttachedAnimations())
                out.push_back({a.label, a.sequenceCount, a.firstSequence});
        return out;
    }

    bool Attach(const std::filesystem::path& path) override {
        model::Actor* hero = playback_.FocusActor();
        io::M3ModelAdapter* m3 = M3AdapterOf(hero);
        if (!m3)
            return false;

        // Loose file first, because that is what the picker hands over; then the
        // scene's provider, so a storage-relative name resolves against the same
        // CASC the model itself came out of.
        std::vector<u8> bytes;
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) {
            std::ifstream in(path, std::ios::binary);
            if (in)
                bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        } else if (auto* provider = service_.Scene().ActiveContentProvider()) {
            if (auto read = provider->ReadFile(ContentRef::FromPath(io::PathToUtf8(path))))
                bytes = std::move(*read);
        }
        if (bytes.empty())
            return false;

        if (!m3->AttachAnimationFile(io::PathToUtf8(path.stem()), bytes))
            return false;

        // Re-bind rather than poke the driver's cache: Bind is what re-reads
        // GetSequences() and rebuilds the pose stages, and the newly merged
        // sequences are invisible to playback until it runs.
        //
        // Not refreshed: the event configs the loader read at spawn time, so an
        // attached file's `Evt_Sound` cues need a reload to fire. Nothing else in
        // the actor depends on the animation file — no geometry, no bones, no
        // palette — because an `.m3a` contributes tracks and nothing more.
        hero->animation.Bind(hero->animation.Source());
        playback_.RefreshSequences(documents_.ActiveState(), hero, /*resetSelection*/ false);
        return true;
    }

    bool Detach(usize index) override {
        model::Actor* hero = playback_.FocusActor();
        io::M3ModelAdapter* m3 = M3AdapterOf(hero);
        if (!m3 || !m3->DetachAnimationFile(index))
            return false;
        hero->animation.Bind(hero->animation.Source());
        // Detaching renumbers everything after the removed file, so the selection
        // is not merely clamped — it is meaningless. Back to the model's first.
        playback_.RefreshSequences(documents_.ActiveState(), hero, /*resetSelection*/ true);
        return true;
    }

    std::vector<Subtrack> SubtracksOf(i32 sequence) const override {
        std::vector<Subtrack> out;
        if (io::M3ModelAdapter* m3 = M3AdapterOf(playback_.FocusActor()))
            for (const auto& s : m3->SubtracksOf(sequence))
                out.push_back({s.name, s.priority, s.concurrent, s.trackCount});
        return out;
    }

private:
    renderer::RenderService& service_;
    DocumentManager& documents_;
    PlaybackController& playback_;
};

} // namespace

std::unique_ptr<Sc2AnimationFiles> MakeSc2AnimationFiles(renderer::RenderService& service,
                                                         DocumentManager& documents,
                                                         PlaybackController& playback) {
    return std::make_unique<Sc2AnimationFilesImpl>(service, documents, playback);
}

} // namespace whiteout::flakes
